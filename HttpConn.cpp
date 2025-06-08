/*
 * HttpConn.cpp 		
 *  Created on: 2025-6-5
 *      Author: kk_lion
 */

#include "HttpConn.h"
#include "HttpParserWrapper.h"

#include "Common.h"
#include "ApiRegister.h"
#include "ApiLogin.h"
#include "ApiMyfiles.h"
#include "ApiSharefiles.h"
#include "ApiDealfile.h"
#include "ApiDealsharefile.h"
#include "ApiSharepicture.h"
#include "ApiMd5.h"
#include "ApiUpload.h"

// 全局HTTP连接映射表，保存所有活跃的HTTP连接
static HttpConnMap_t g_http_conn_map;

extern string strMsfsUrl;
extern string strDiscovery;

// 连接句柄生成器，防止socket句柄复用导致冲突
static uint32_t g_conn_handle_generator = 0;

// 通过连接句柄查找HTTP连接对象
CHttpConn *FindHttpConnByHandle(uint32_t conn_handle)
{
    CHttpConn *pConn = NULL;
    HttpConnMap_t::iterator it = g_http_conn_map.find(conn_handle);
    if (it != g_http_conn_map.end())
    {
        pConn = it->second;
    }
    return pConn;
}

// 网络事件回调函数，处理读写、关闭等事件
void httpconn_callback(void *callback_data, uint8_t msg, uint32_t handle, uint32_t uParam, void *pParam)
{
    NOTUSED_ARG(uParam);
    NOTUSED_ARG(pParam);

    // 将void*类型的callback_data转换为uint32_t类型的连接句柄
    uint32_t conn_handle = *((uint32_t *)(&callback_data));
    CHttpConn *pConn = FindHttpConnByHandle(conn_handle);
    if (!pConn)
    {
        return;
    }

    switch (msg)
    {
    case NETLIB_MSG_READ:
        pConn->OnRead(); // 处理读事件
        break;
    case NETLIB_MSG_WRITE:
        pConn->OnWrite(); // 处理写事件
        break;
    case NETLIB_MSG_CLOSE:
        pConn->OnClose(); // 处理关闭事件
        break;
    default:
        LOG_ERROR << "!!!httpconn_callback error msg:" << msg;
        break;
    }
}

// 定时器回调函数，定期检查连接是否超时
void http_conn_timer_callback(void *callback_data, uint8_t msg, uint32_t handle, void *pParam)
{
    CHttpConn *pConn = NULL;
    HttpConnMap_t::iterator it, it_old;
    uint64_t cur_time = get_tick_count();

    for (it = g_http_conn_map.begin(); it != g_http_conn_map.end();)
    {
        it_old = it;
        it++;
        pConn = it_old->second;
        pConn->OnTimer(cur_time); // 检查超时
    }
}

// 初始化HTTP连接模块，注册定时器
void init_http_conn()
{
    netlib_register_timer(http_conn_timer_callback, NULL, 1000);
}

//////////////////////////
// HTTP连接类构造函数
CHttpConn::CHttpConn()
{
    m_busy = false;
    m_sock_handle = NETLIB_INVALID_HANDLE;
    m_state = CONN_STATE_IDLE;

    m_last_send_tick = m_last_recv_tick = get_tick_count();
    m_conn_handle = ++g_conn_handle_generator;
    if (m_conn_handle == 0)
    {
        m_conn_handle = ++g_conn_handle_generator;
    }

    // LOG_INFO << "CHttpConn, handle = " << m_conn_handle;
}

// 析构函数
CHttpConn::~CHttpConn()
{
    // LOG_INFO << "~CHttpConn, handle = " << m_conn_handle;
}

// 发送数据到客户端
int CHttpConn::Send(void *data, int len)
{
    m_last_send_tick = get_tick_count();

    if (m_busy)
    {
        m_out_buf.Write(data, len); // 如果忙则写入输出缓冲区
        return len;
    }

    int ret = netlib_send(m_sock_handle, data, len);
    if (ret < 0)
        ret = 0;

    if (ret < len)
    {
        m_out_buf.Write((char *)data + ret, len - ret); // 未全部发送则写入缓冲区
        m_busy = true;
        LOG_INFO << "not send all, remain=" << m_out_buf.GetWriteOffset();
    }
    else
    {
        OnWriteComlete(); // 全部发送完毕
    }

    return len;
}

// 关闭连接
void CHttpConn::Close()
{
    m_state = CONN_STATE_CLOSED;

    g_http_conn_map.erase(m_conn_handle); // 从全局映射表移除
    netlib_close(m_sock_handle);          // 关闭socket

    ReleaseRef(); // 释放引用
}

// 新连接建立时调用
void CHttpConn::OnConnect(net_handle_t handle)
{
    // LOG_INFO << "CHttpConn, handle = " << handle;
    m_sock_handle = handle;
    m_state = CONN_STATE_CONNECTED;
    g_http_conn_map.insert(make_pair(m_conn_handle, this)); // 加入全局映射表

    // 设置网络事件回调
    netlib_option(handle, NETLIB_OPT_SET_CALLBACK, (void *)httpconn_callback);
    netlib_option(handle, NETLIB_OPT_SET_CALLBACK_DATA, reinterpret_cast<void *>(m_conn_handle));
    netlib_option(handle, NETLIB_OPT_GET_REMOTE_IP, (void *)&m_peer_ip);
}

// 处理读事件
void CHttpConn::OnRead()
{
    for (;;)
    {
        uint32_t free_buf_len = m_in_buf.GetAllocSize() - m_in_buf.GetWriteOffset();
        if (free_buf_len < READ_BUF_SIZE + 1)
            m_in_buf.Extend(READ_BUF_SIZE + 1);

        int ret = netlib_recv(m_sock_handle, m_in_buf.GetBuffer() + m_in_buf.GetWriteOffset(), READ_BUF_SIZE);
        if (ret <= 0)
            break;

        m_in_buf.IncWriteOffset(ret);

        m_last_recv_tick = get_tick_count();
    }

    // 每次请求对应一个HTTP连接，所以读完数据后，不用在同一个连接里面准备读取下个请求
    char *in_buf = (char *)m_in_buf.GetBuffer();
    uint32_t buf_len = m_in_buf.GetWriteOffset();
    in_buf[buf_len] = '\0';

    // 如果buf_len 过长可能是受到攻击，则断开连接
    // 正常的url最大长度为2048，我们接受的所有数据长度不得大于2K
    if (buf_len > 2048)
    {
        LOG_ERROR << "get too much data: " << in_buf;
        Close();
        return;
    }

    LOG_DEBUG << "buf_len: " << buf_len << ", m_conn_handle: " << m_conn_handle << ", in_buf: " << in_buf;
    // 解析http数据
    m_cHttpParser.ParseHttpContent(in_buf, buf_len);
    if (m_cHttpParser.IsReadAll())
    {
        string url = m_cHttpParser.GetUrl();
        string content = m_cHttpParser.GetBodyContent();
        LOG_INFO << "url: " << url; // for debug
        // 根据url路由到不同的处理函数
        if (strncmp(url.c_str(), "/api/reg", 8) == 0)
        { // 注册
            _HandleRegisterRequest(url, content);
        }
        else if (strncmp(url.c_str(), "/api/login", 10) == 0)
        { // 登录
            _HandleLoginRequest(url, content);
        }
        else if (strncmp(url.c_str(), "/api/myfiles", 10) == 0)
        { //
            _HandleMyfilesRequest(url, content);
        }
        else if (strncmp(url.c_str(), "/api/sharefiles", 15) == 0)
        { //
            _HandleSharefilesRequest(url, content);
        }
        else if (strncmp(url.c_str(), "/api/dealfile", 13) == 0)
        { //
            _HandleDealfileRequest(url, content);
        }
        else if (strncmp(url.c_str(), "/api/dealsharefile", 18) == 0)
        { //
            _HandleDealsharefileRequest(url, content);
        }
        else if (strncmp(url.c_str(), "/api/sharepic", 13) == 0)
        {											  //
            _HandleSharepictureRequest(url, content); // 处理
        }
        else if (strncmp(url.c_str(), "/api/md5", 8) == 0)
        {									 //
            _HandleMd5Request(url, content); // 处理
        }
        else if (strncmp(url.c_str(), "/api/upload", 11) == 0)
        { // 上传
            _HandleUploadRequest(url, content);
        }
        else
        {
            LOG_ERROR << "url unknown, url= " << url;
            Close();
        }
    }
}

// 处理写事件
void CHttpConn::OnWrite()
{
    if (!m_busy)
        return;

    // LOG_INFO << "send: " << m_out_buf.GetWriteOffset();
    int ret = netlib_send(m_sock_handle, m_out_buf.GetBuffer(), m_out_buf.GetWriteOffset());
    if (ret < 0)
        ret = 0;

    int out_buf_size = (int)m_out_buf.GetWriteOffset();

    m_out_buf.Read(NULL, ret);

    if (ret < out_buf_size)
    {
        m_busy = true;
        LOG_INFO << "not send all, remain = " << m_out_buf.GetWriteOffset();
    }
    else
    {
        OnWriteComlete();
        m_busy = false;
    }
}

// 处理关闭事件
void CHttpConn::OnClose()
{
    Close();
}

// 定时器处理，检测连接是否超时
void CHttpConn::OnTimer(uint64_t curr_tick)
{
    if (curr_tick > m_last_recv_tick + HTTP_CONN_TIMEOUT)
    {
        LOG_WARN  << "HttpConn timeout, handle=" << m_conn_handle;
        Close();
    }
}

/*
OnRead, buf_len=1321, conn_handle=2, POST /api/upload HTTP/1.0
Host: 127.0.0.1:8081
Connection: close
Content-Length: 722
Accept: application/json, text/plain,
User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/106.0.0.0 Safari/537.36 Edg/106.0.1370.52
Content-Type: multipart/form-data; boundary=----WebKitFormBoundaryjWE3qXXORSg2hZiB
Origin: http://114.215.169.66
Referer: http://114.215.169.66/myFiles
Accept-Encoding: gzip, deflate
Accept-Language: zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6
Cookie: userName=qingfuliao; token=e4252ae6e49176d51a5e87b41b6b9312

------WebKitFormBoundaryjWE3qXXORSg2hZiB
Content-Disposition: form-data; name="file_name"

config.ini
------WebKitFormBoundaryjWE3qXXORSg2hZiB
Content-Disposition: form-data; name="file_content_type"

application/octet-stream
------WebKitFormBoundaryjWE3qXXORSg2hZiB
Content-Disposition: form-data; name="file_path"

/root/tmp/5/0034880075
------WebKitFormBoundaryjWE3qXXORSg2hZiB
Content-Disposition: form-data; name="file_md5"

10f06f4707e9d108e9a9838de0f8ee33
------WebKitFormBoundaryjWE3qXXORSg2hZiB
Content-Disposition: form-data; name="file_size"

20
------WebKitFormBoundaryjWE3qXXORSg2hZiB
Content-Disposition: form-data; name="user"

qingfuliao
------WebKitFormBoundaryjWE3qXXORSg2hZiB--
*/

// 处理文件上传请求 /api/upload
int CHttpConn::_HandleUploadRequest(string &url, string &post_data)
{
    string str_json;
    // 调用上传API，处理上传逻辑，返回json字符串
    int ret = ApiUpload(url, post_data, str_json);
    // 构造HTTP响应内容
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX];
    uint32_t nLen = str_json.length();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));   // 发送响应数据给客户端
    return 0;
}

// 账号注册处理 /api/reg
int CHttpConn::_HandleRegisterRequest(string &url, string &post_data)
{
    string str_json;
    // 调用注册API，处理注册逻辑，返回json字符串
    int ret = ApiRegisterUser(url, post_data, str_json);

    // 构造HTTP响应内容
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX];
    uint32_t nLen = str_json.length();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));
    return 0;
}

// 账号登陆处理 /api/login
int CHttpConn::_HandleLoginRequest(string &url, string &post_data)
{
    string str_json;
    // 调用登录API，处理登录逻辑，返回json字符串
    int ret = ApiUserLogin(url, post_data, str_json);
    // 构造HTTP响应内容
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX];
    uint32_t nLen = str_json.length();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));

    return 0;
}

// 处理文件操作请求 /api/dealfile
int CHttpConn::_HandleDealfileRequest(string &url, string &post_data)
{
    string str_json;
    int ret = ApiDealfile(url, post_data, str_json);
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX];
    uint32_t nLen = str_json.length();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));
    return 0;
}

// 处理分享文件操作请求 /api/dealsharefile
int CHttpConn::_HandleDealsharefileRequest(string &url, string &post_data)
{
    string str_json;
    int ret = ApiDealsharefile(url, post_data, str_json);
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX];
    uint32_t nLen = str_json.length();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));

    return 0;
}

// 处理MD5校验请求 /api/md5
int CHttpConn::_HandleMd5Request(string &url, string &post_data)
{
    string str_json;
    int ret = ApiMd5(url, post_data, str_json);
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX]; // 注意buffer的长度
    uint32_t nLen = str_json.length();
    LOG_INFO << "json size:" << str_json.size();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));

    return 0;
}

// 处理获取我的文件请求 /api/myfiles
int CHttpConn::_HandleMyfilesRequest(string &url, string &post_data)
{
    string str_json;
    int ret = ApiMyfiles(url, post_data, str_json);
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX]; // 注意buffer的长度
    uint32_t nLen = str_json.length();
    // LOG_INFO << "json size:" << str_json.size();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));

    return 0;
}

// 处理获取分享文件请求 /api/sharefiles
int CHttpConn::_HandleSharefilesRequest(string &url, string &post_data)
{
    string str_json;
    int ret = ApiSharefiles(url, post_data, str_json);
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX];
    uint32_t nLen = str_json.length();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));
    return 0;
}

// 处理获取分享图片请求 /api/sharepic
int CHttpConn::_HandleSharepictureRequest(string &url, string &post_data)
{
    string str_json;
    int ret = ApiSharepicture(url, post_data, str_json);
    char *szContent = new char[HTTP_RESPONSE_HTML_MAX];
    uint32_t nLen = str_json.length();
    snprintf(szContent, HTTP_RESPONSE_HTML_MAX, HTTP_RESPONSE_HTML, nLen, str_json.c_str());
    ret = Send((void *)szContent, strlen(szContent));
    return 0;
}

// 写完成回调，关闭连接
void CHttpConn::OnWriteComlete()
{
    // LOG_INFO  << "write complete";
    Close();
}
