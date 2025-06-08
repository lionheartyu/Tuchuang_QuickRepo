/*
 * HttpConn.h
 *
 *  Created on: 2013-9-29
 *      Author: ziteng
 */

#ifndef __HTTP_CONN_H__
#define __HTTP_CONN_H__

#include "netlib.h"
#include "util.h"
#include "HttpParserWrapper.h"

// HTTP连接超时时间（毫秒）
#define HTTP_CONN_TIMEOUT			60000

// 读取缓冲区大小
#define READ_BUF_SIZE	2048

// HTTP响应模板（返回JSON格式数据）
#define HTTP_RESPONSE_HTML          "HTTP/1.1 200 OK\r\n"\
                                    "Connection:close\r\n"\
                                    "Content-Length:%d\r\n"\
                                    "Content-Type:application/json;charset=utf-8\r\n\r\n%s"

// HTTP响应内容最大长度
#define HTTP_RESPONSE_HTML_MAX      4096

// 连接状态枚举
enum {
    CONN_STATE_IDLE,      // 空闲
    CONN_STATE_CONNECTED, // 已连接
    CONN_STATE_OPEN,      // 已打开
    CONN_STATE_CLOSED,    // 已关闭
};

// HTTP连接类，继承自引用计数基类
class CHttpConn : public CRefObject
{
public:
    CHttpConn();              // 构造函数
    virtual ~CHttpConn();     // 析构函数

    uint32_t GetConnHandle() { return m_conn_handle; } // 获取连接句柄
    char* GetPeerIP() { return (char*)m_peer_ip.c_str(); } // 获取对端IP

    int Send(void* data, int len); // 发送数据

    void Close();                  // 关闭连接
    void OnConnect(net_handle_t handle); // 新连接建立时调用
    void OnRead();                 // 处理读事件
    void OnWrite();                // 处理写事件
    void OnClose();                // 处理关闭事件
    void OnTimer(uint64_t curr_tick); // 定时器回调，检测超时
    void OnWriteComlete();         // 写完成回调

private:
    // 处理文件上传请求
    int _HandleUploadRequest(string& url, string& post_data);
    // 账号注册处理
    int _HandleRegisterRequest(string& url, string& post_data);
    // 账号登陆处理
    int _HandleLoginRequest(string& url, string& post_data);
    // 文件操作处理
    int _HandleDealfileRequest(string& url, string& post_data);
    // 分享文件操作处理
    int _HandleDealsharefileRequest(string& url, string& post_data);
    // MD5校验处理
    int _HandleMd5Request(string& url, string& post_data);
    // 获取我的文件处理
    int _HandleMyfilesRequest(string& url, string& post_data);
    // 获取分享文件处理
    int _HandleSharefilesRequest(string& url, string& post_data);
    // 获取分享图片处理
    int _HandleSharepictureRequest(string& url, string& post_data);

protected:
    net_handle_t	m_sock_handle;   // socket句柄
    uint32_t		m_conn_handle;   // 连接句柄
    bool			m_busy;          // 是否繁忙

    uint32_t        m_state;         // 连接状态
    std::string		m_peer_ip;       // 对端IP
    uint16_t		m_peer_port;     // 对端端口
    CSimpleBuffer	m_in_buf;        // 输入缓冲区
    CSimpleBuffer	m_out_buf;       // 输出缓冲区

    uint64_t		m_last_send_tick; // 上次发送时间
    uint64_t		m_last_recv_tick; // 上次接收时间
    
    CHttpParserWrapper m_cHttpParser; // HTTP协议解析器
};

// 连接句柄到连接对象的映射表
typedef hash_map<uint32_t, CHttpConn*> HttpConnMap_t;

// 通过句柄查找HTTP连接对象
CHttpConn* FindHttpConnByHandle(uint32_t handle);

// 初始化HTTP连接模块
void init_http_conn();

#endif /* IMCONN_H_ */
