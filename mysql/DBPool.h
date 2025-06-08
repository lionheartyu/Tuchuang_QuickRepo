#ifndef DBPOOL_H_
#define DBPOOL_H_



#include <iostream>
#include <list>
#include <mutex>
#include <condition_variable>
#include <map>
#include <stdint.h>

#include <mysql.h>

#define MAX_ESCAPE_STRING_LEN 10240

using namespace std;


// ┌───────────────────────────────────────────────┐
// │                CDBManager（管理器）           │
// │  ┌─────────────────────────────────────────┐  │
// │  │  m_dbpool_map: 连接池名称 → CDBPool*    │  │
// │  └─────────────────────────────────────────┘  │
// │                ▲                              │
// │                │                              │
// │   ┌────────────┴────────────┐                 │
// │   │                         │                 │
// │   ▼                         ▼                 │
// │ ┌──────────────┐      ┌──────────────┐        │
// │ │ CDBPool主库  │      │ CDBPool从库  │ ...    │
// │ │ (tuchuang_master)   │ (tuchuang_slave)      │
// │ └──────────────┘      └──────────────┘        │
// │   ▲         ▲             ▲         ▲         │
// │   │         │             │         │         │
// │   │         │             │         │         │
// │   ▼         ▼             ▼         ▼         │
// │ ┌────┐  ┌────┐        ┌────┐  ┌────┐         │
// │ │CDBConn│...│CDBConn│  │CDBConn│...│CDBConn│ │
// │ └────┘  └────┘        └────┘  └────┘         │
// │   ▲         ▲             ▲         ▲         │
// │   │         │             │         │         │
// │   ▼         ▼             ▼         ▼         │
// │ MySQL连接 MySQL连接   MySQL连接 MySQL连接      │
// │   (主库)    (主库)      (从库)    (从库)      │
// └───────────────────────────────────────────────┘

// 说明：
// - CDBManager 统一管理多个 CDBPool（主库、从库等）。
// - 每个 CDBPool 维护一个数据库连接池（多个 CDBConn）。
// - 每个 CDBConn 封装一个实际的 MySQL 连接。
// - 程序通过 CDBManager 获取/归还连接，实现高效复用和并发访问。

// 查询结果集封装类，select 查询时使用
class CResultSet
{
public:
    CResultSet(MYSQL_RES *res);      // 构造函数，传入MySQL查询结果
    virtual ~CResultSet();           // 析构函数，释放资源

    bool Next();                     // 移动到下一行
    int GetInt(const char *key);     // 获取整型字段值
    char *GetString(const char *key);// 获取字符串字段值

private:
    int _GetIndex(const char *key);  // 获取字段索引

    MYSQL_RES *m_res;                // MySQL查询结果指针
    MYSQL_ROW m_row;                 // 当前行数据
    map<string, int> m_key_map;      // 字段名到索引的映射
};

// 预处理语句封装类，插入数据时使用
class CPrepareStatement
{
public:
    CPrepareStatement();
    virtual ~CPrepareStatement();

    bool Init(MYSQL *mysql, string &sql); // 初始化预处理语句

    void SetParam(uint32_t index, int &value);         // 设置整型参数
    void SetParam(uint32_t index, uint32_t &value);    // 设置无符号整型参数
    void SetParam(uint32_t index, string &value);      // 设置字符串参数
    void SetParam(uint32_t index, const string &value);// 设置字符串参数

    bool ExecuteUpdate();              // 执行更新
    uint32_t GetInsertId();            // 获取插入ID

private:
    MYSQL_STMT *m_stmt;                // MySQL预处理语句指针
    MYSQL_BIND *m_param_bind;          // 参数绑定数组
    uint32_t m_param_cnt;              // 参数个数
};

class CDBPool;

// 数据库连接封装类
class CDBConn
{
public:
    CDBConn(CDBPool *pDBPool);         // 构造函数
    virtual ~CDBConn();                // 析构函数
    int Init();                        // 初始化连接

    // 数据库操作接口
    bool ExecuteCreate(const char *sql_query);     // 创建表
    bool ExecuteDrop(const char *sql_query);       // 删除表
    CResultSet *ExecuteQuery(const char *sql_query);// 查询
    bool ExecutePassQuery(const char *sql_query);  // 执行无返回的查询
    bool ExecuteUpdate(const char *sql_query, bool care_affected_rows = true); // 更新
    uint32_t GetInsertId();                        // 获取插入ID

    // 事务相关
    bool StartTransaction();   // 开启事务
    bool Commit();             // 提交事务
    bool Rollback();           // 回滚事务

    // 获取连接信息
    const char *GetPoolName();
    MYSQL *GetMysql() { return m_mysql; }
    int GetRowNum() { return row_num; }

private:
    int row_num = 0;           // 查询结果行数
    CDBPool *m_pDBPool;        // 所属连接池
    MYSQL *m_mysql;            // MySQL连接指针
    char m_escape_string[MAX_ESCAPE_STRING_LEN + 1]; // 转义字符串缓冲区
};

// 数据库连接池管理类
class CDBPool
{ // 只是负责管理连接CDBConn，真正干活的是CDBConn
public:
    CDBPool() {} // 构造函数
    CDBPool(const char *pool_name, const char *db_server_ip, uint16_t db_server_port,
            const char *username, const char *password, const char *db_name,
            int max_conn_cnt);
    virtual ~CDBPool();

    int Init();									  // 初始化连接池
    CDBConn *GetDBConn(const int timeout_ms = 0); // 获取一个连接
    void RelDBConn(CDBConn *pConn);				  // 归还连接

    // 获取配置信息
    const char *GetPoolName() { return m_pool_name.c_str(); }
    const char *GetDBServerIP() { return m_db_server_ip.c_str(); }
    uint16_t GetDBServerPort() { return m_db_server_port; }
    const char *GetUsername() { return m_username.c_str(); }
    const char *GetPasswrod() { return m_password.c_str(); }
    const char *GetDBName() { return m_db_name.c_str(); }

private:
    string m_pool_name;			 // 连接池名称
    string m_db_server_ip;		 // 数据库ip
    uint16_t m_db_server_port;	 // 数据库端口
    string m_username;			 // 用户名
    string m_password;			 // 用户密码
    string m_db_name;			 // db名称
    int m_db_cur_conn_cnt;		 // 当前启用的连接数量
    int m_db_max_conn_cnt;		 // 最大连接数量
    list<CDBConn *> m_free_list; // 空闲的连接

    list<CDBConn *> m_used_list; // 已被使用的连接
    std::mutex m_mutex;          // 互斥锁
    std::condition_variable m_cond_var; // 条件变量
    bool m_abort_request = false;// 是否中止请求
};

// 数据库连接池管理器（支持多实例，如主从）
class CDBManager
{
public:
    virtual ~CDBManager();

    static CDBManager *getInstance(); // 单例获取

    int Init();                       // 初始化所有连接池

    CDBConn *GetDBConn(const char *dbpool_name); // 获取指定连接池的连接
    void RelDBConn(CDBConn *pConn);              // 归还连接

private:
    CDBManager();

private:
    static CDBManager *s_db_manager;             // 单例指针
    map<string, CDBPool *> m_dbpool_map;         // 连接池映射
};

// 自动归还数据库连接的RAII类
class AutoRelDBCon
{
public:
    AutoRelDBCon(CDBManager *manger, CDBConn *conn) : manger_(manger), conn_(conn) {}
    ~AutoRelDBCon()
    {
        if (manger_)
        {
            manger_->RelDBConn(conn_);
        }
    } // 析构时自动归还连接
private:
    CDBManager *manger_ = NULL;
    CDBConn *conn_ = NULL;
};

// 自动归还数据库连接的宏，作用域结束自动释放
#define AUTO_REAL_DBCONN(m, c) AutoRelDBCon autoreldbconn(m, c)

#endif /* DBPOOL_H_ */
