/**
 * 头文件包含规范
 * 1.本类的声明（第一个包含本类h文件，有效减少依赖）
 * 2.C系统文件
 * 3.C++系统文件
 * 4.其他库头文件
 * 5.本项目内头文件
*/

// using std::string; // 可以在整个cc文件和h文件内使用using， 禁止使用using namespace xx污染命名空间

#include "netlib.h"         // 网络库，提供网络通信功能
#include "ConfigFileReader.h" // 配置文件读取器
#include "HttpConn.h"       // HTTP连接处理类
#include "util.h"           // 通用工具函数
#include "DBPool.h"         // MySQL数据库连接池
#include "CachePool.h"      // Redis缓存连接池

#include "Logging.h"        // 日志系统
#include "AsyncLogging.h"   // 异步日志系统
#include "ApiUpload.h"      // 文件上传API
#include "ApiDealfile.h"    // 文件处理API

// 日志文件大小限制：1MB
off_t kRollSize = 1 * 1000 * 1000;    
static AsyncLogging *g_asyncLog = NULL; // 全局异步日志对象指针

/**
 * 异步日志输出回调函数
 * @param msg 日志消息
 * @param len 消息长度
 */
static void asyncOutput(const char *msg, int len)
{
    g_asyncLog->append(msg, len);
}

/**
 * 初始化日志系统
 * @return 0:成功 -1:失败
 */
int InitLog()
{
    printf("pid = %d\n", getpid()); // 打印进程ID

    char name[256] = "tuchuang";     // 日志文件名前缀
    
    // 创建异步日志对象
    // 参数：日志文件名，文件大小限制(1M)，刷盘间隔(1秒)
    AsyncLogging log(::basename(name), kRollSize, 1);  
    
    log.start();        // 启动日志写入线程

    LOG_INFO << "InitLog ok"; // 记录日志初始化完成

    return 0;
}

/**
 * HTTP连接回调函数
 * 当有新的HTTP连接时，网络库会调用此函数
 * @param callback_data 回调数据（未使用）
 * @param msg 消息类型
 * @param handle 连接句柄
 * @param pParam 参数（未使用）
 */
void http_callback(void* callback_data, uint8_t msg, uint32_t handle, void* pParam)
{
    // 检查消息类型是否为新连接
    if (msg == NETLIB_MSG_CONNECT)
    {
        // 注意：这里new了对象却没有显式释放
        // 实际上对象在连接关闭时使用delete this的方式释放自己
        CHttpConn* pConn = new CHttpConn();
        pConn->OnConnect(handle); // 处理新连接
    }
    else
    {
        LOG_ERROR << "!!!error msg: " << msg; // 记录错误消息
    }
}

/**
 * 主函数 - 程序入口点
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 0:成功 其他:失败
 */
int main(int argc, char* argv[])
{
    // 忽略SIGPIPE信号，防止因网络断开导致程序崩溃
    signal(SIGPIPE, SIG_IGN);

    // 步骤1：初始化日志系统
    InitLog();

    // 步骤2：初始化Redis缓存连接池
    // 内部会读取配置文件tc_http_server.conf获取Redis连接信息
    CacheManager* pCacheManager = CacheManager::getInstance();
    if (!pCacheManager) {
        LOG_ERROR << "CacheManager init failed";
        std::cout << "CacheManager init failed";
        return -1;
    }

    // 步骤3：初始化MySQL数据库连接池
    // 内部会读取配置文件获取数据库连接信息
    CDBManager* pDBManager = CDBManager::getInstance();
    if (!pDBManager) {
        LOG_ERROR << "DBManager init failed";
        std::cout << "DBManager init failed";
        return -1;
    }

    // 步骤4：读取配置文件
    CConfigFileReader config_file("tc_http_server.conf");

    // 获取HTTP服务器配置
    char* http_listen_ip = config_file.GetConfigName("HttpListenIP");   // HTTP监听IP
    char* str_http_port = config_file.GetConfigName("HttpPort");        // HTTP监听端口

    // 获取FastDFS和Web服务器配置
    char* dfs_path_client = config_file.GetConfigName("dfs_path_client");           // FastDFS客户端配置路径
    char* web_server_ip = config_file.GetConfigName("web_server_ip");               // Web服务器IP
    char* web_server_port = config_file.GetConfigName("web_server_port");           // Web服务器端口
    char* storage_web_server_ip = config_file.GetConfigName("storage_web_server_ip"); // 存储服务器IP
    char* storage_web_server_port = config_file.GetConfigName("storage_web_server_port"); // 存储服务器端口

    // 步骤5：初始化API模块，传递配置参数
    ApiUploadInit(dfs_path_client, web_server_ip, web_server_port, storage_web_server_ip, storage_web_server_port);
    ApiDealfileInit(dfs_path_client);

    // 步骤6：检查必要的配置项
    if (!http_listen_ip || !str_http_port) {
        printf("config item missing, exit... ip:%s, port:%s", http_listen_ip, str_http_port);
        return -1;
    }

    // 转换端口号为整数
    uint16_t http_port = atoi(str_http_port);

    // 步骤7：初始化网络库
    int ret = netlib_init();
    if (ret == NETLIB_ERROR)
        return ret; 
    
    // 步骤8：设置网络监听
    // 支持多个IP地址监听，用分号分隔
    CStrExplode http_listen_ip_list(http_listen_ip, ';');
    for (uint32_t i = 0; i < http_listen_ip_list.GetItemCnt(); i++) {
        // 在指定IP和端口上监听HTTP连接，设置回调函数
        ret = netlib_listen(http_listen_ip_list.GetItem(i), http_port, http_callback, NULL);
        if (ret == NETLIB_ERROR)
            return ret;
    }
    
    // 打印服务器启动信息
    printf("server start listen on:For http:%s:%d\n", http_listen_ip, http_port);
    
    // 步骤9：初始化HTTP连接相关资源
    init_http_conn();

    printf("now enter the event loop...\n");
    
    // 步骤10：写入进程ID到文件（用于进程管理）
    writePid();

    // 步骤11：进入事件循环，开始处理网络事件
    // 这是一个阻塞调用，程序将在此处理所有网络请求
    netlib_eventloop();
    
    return 0;
}
