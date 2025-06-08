#include "DBPool.h"
#include <string.h>

#include "ConfigFileReader.h"
#include "Logging.h"


// +-------------------------------------------------------------+
// |                        CDBManager (单例)                   |
// +-------------------------------------------------------------+
// | - static CDBManager* s_db_manager                           |
// | - map<string, CDBPool*> m_dbpool_map                        |
// +-------------------------------------------------------------+
// | + getInstance()                                             |
// | + Init()                                                    |
// | + GetDBConn(const char* dbpool_name) : CDBConn*             |
// | + RelDBConn(CDBConn* pConn)                                 |
// +-------------------------------------------------------------+
//                 | 1
//                 |-----------------------------+
//                 |                             |
//                 v                             |
// +-------------------------------------------------------------+
// |                        CDBPool                              |
// +-------------------------------------------------------------+
// | - string m_pool_name                                        |
// | - string m_db_server_ip                                     |
// | - uint16_t m_db_server_port                                 |
// | - string m_username                                         |
// | - string m_password                                         |
// | - string m_db_name                                          |
// | - int m_db_max_conn_cnt                                     |
// | - int m_db_cur_conn_cnt                                     |
// | - list<CDBConn*> m_free_list                                |
// | - std::mutex m_mutex                                        |
// | - std::condition_variable m_cond_var                        |
// | - bool m_abort_request                                      |
// +-------------------------------------------------------------+
// | + Init()                                                    |
// | + GetDBConn(int timeout_ms=0) : CDBConn*                    |
// | + RelDBConn(CDBConn* pConn)                                 |
// | + GetPoolName() : const char*                               |
// | + GetDBServerIP()/GetUsername()/GetPasswrod()/GetDBName()   |
// | + GetDBServerPort()                                         |
// +-------------------------------------------------------------+
//                 | 1
//                 |-----------------------------+
//                 |                             |
//                 v                             |
// +-------------------------------------------------------------+
// |                        CDBConn                              |
// +-------------------------------------------------------------+
// | - CDBPool* m_pDBPool                                        |
// | - MYSQL* m_mysql                                            |
// | - int row_num                                               |
// +-------------------------------------------------------------+
// | + Init()                                                    |
// | + ExecuteQuery(const char* sql) : CResultSet*               |
// | + ExecuteUpdate(const char* sql, bool care_affected_rows)   |
// | + ExecuteCreate/ExecuteDrop/ExecutePassQuery                |
// | + StartTransaction()/Commit()/Rollback()                    |
// | + GetInsertId()                                             |
// | + GetPoolName()                                             |
// +-------------------------------------------------------------+
//                 | 1
//                 |-----------------------------+
//                 |                             |
//                 v                             |
// +-------------------------------------------------------------+
// |                    CPrepareStatement                        |
// +-------------------------------------------------------------+
// | - MYSQL_STMT* m_stmt                                        |
// | - MYSQL_BIND* m_param_bind                                  |
// | - uint32_t m_param_cnt                                      |
// +-------------------------------------------------------------+
// | + Init(MYSQL*, string&)                                     |
// | + SetParam(index, value)                                    |
// | + ExecuteUpdate()                                           |
// | + GetInsertId()                                             |
// +-------------------------------------------------------------+

// +-------------------------------------------------------------+
// |                        CResultSet                           |
// +-------------------------------------------------------------+
// | - MYSQL_RES* m_res                                          |
// | - MYSQL_ROW m_row                                           |
// | - map<string, int> m_key_map                                |
// +-------------------------------------------------------------+
// | + Next()                                                    |
// | + GetInt(const char* key)                                   |
// | + GetString(const char* key)                                |
// +-------------------------------------------------------------+
#define MIN_DB_CONN_CNT 1
#define MAX_DB_CONN_FAIL_NUM 10

CDBManager *CDBManager::s_db_manager = NULL;

// 结果集构造函数，初始化字段名到索引的映射
CResultSet::CResultSet(MYSQL_RES *res)
{
    m_res = res;

    // map table field key to index in the result array
    int num_fields = mysql_num_fields(m_res); // 获取结果集中的字段数
    MYSQL_FIELD *fields = mysql_fetch_fields(m_res); // 获取所有字段信息
    for (int i = 0; i < num_fields; i++)
    {
        m_key_map.insert(make_pair(fields[i].name, i)); // 建立字段名到索引的映射
    }
}

// 结果集析构函数，释放资源
CResultSet::~CResultSet()
{
    if (m_res)
    {
        mysql_free_result(m_res);
        m_res = NULL;
    }
}

// 获取下一行数据
bool CResultSet::Next()
{
    m_row = mysql_fetch_row(m_res); // 获取下一行
    if (m_row)
    {
        return true;
    }
    else
    {
        return false;
    }
}

// 获取字段名对应的索引
int CResultSet::_GetIndex(const char *key)
{
    map<string, int>::iterator it = m_key_map.find(key);
    if (it == m_key_map.end())
    {
        return -1;
    }
    else
    {
        return it->second;
    }
}

// 获取整型字段值
int CResultSet::GetInt(const char *key)
{
    int idx = _GetIndex(key);  // 查找列的索引
    if (idx == -1)
    {
        return 0;
    }
    else
    {
        return atoi(m_row[idx]); // 转换为int
    }
}

// 获取字符串字段值
char *CResultSet::GetString(const char *key)
{
    int idx = _GetIndex(key);
    if (idx == -1)
    {
        return NULL;
    }
    else
    {
        return m_row[idx]; // 返回字符串
    }
}

/////////////////////////////////////////
// 预处理语句构造函数
CPrepareStatement::CPrepareStatement()
{
    m_stmt = NULL;
    m_param_bind = NULL;
    m_param_cnt = 0;
}

// 预处理语句析构函数，释放资源
CPrepareStatement::~CPrepareStatement()
{
    if (m_stmt)
    {
        mysql_stmt_close(m_stmt);
        m_stmt = NULL;
    }

    if (m_param_bind)
    {
        delete[] m_param_bind;
        m_param_bind = NULL;
    }
}

// 初始化预处理语句
bool CPrepareStatement::Init(MYSQL *mysql, string &sql)
{
    mysql_ping(mysql); // 检查连接有效性，自动重连

    m_stmt = mysql_stmt_init(mysql);
    if (!m_stmt)
    {
        LOG_ERROR << "mysql_stmt_init failed";
        return false;
    }

    if (mysql_stmt_prepare(m_stmt, sql.c_str(), sql.size()))
    {
        LOG_ERROR << "mysql_stmt_prepare failed: " << mysql_stmt_error(m_stmt);
        return false;
    }

    m_param_cnt = mysql_stmt_param_count(m_stmt);
    if (m_param_cnt > 0)
    {
        m_param_bind = new MYSQL_BIND[m_param_cnt];
        if (!m_param_bind)
        {
            LOG_ERROR << "new failed";
            return false;
        }

        memset(m_param_bind, 0, sizeof(MYSQL_BIND) * m_param_cnt);
    }

    return true;
}

// 设置int参数
void CPrepareStatement::SetParam(uint32_t index, int &value)
{
    if (index >= m_param_cnt)
    {
        LOG_ERROR << "index too large: " << index;
        return;
    }

    m_param_bind[index].buffer_type = MYSQL_TYPE_LONG;
    m_param_bind[index].buffer = &value;
}

// 设置uint32参数
void CPrepareStatement::SetParam(uint32_t index, uint32_t &value)
{
    if (index >= m_param_cnt)
    {
        LOG_ERROR <<  "index too large: " << index;
        return;
    }

    m_param_bind[index].buffer_type = MYSQL_TYPE_LONG;
    m_param_bind[index].buffer = &value;
}

// 设置string参数
void CPrepareStatement::SetParam(uint32_t index, string &value)
{
    if (index >= m_param_cnt)
    {
        LOG_ERROR << "index too large: " << index;
        return;
    }

    m_param_bind[index].buffer_type = MYSQL_TYPE_STRING;
    m_param_bind[index].buffer = (char *)value.c_str();
    m_param_bind[index].buffer_length = value.size();
}

// 设置const string参数
void CPrepareStatement::SetParam(uint32_t index, const string &value)
{
    if (index >= m_param_cnt)
    {
        LOG_ERROR << "index too large: " << index;
        return;
    }

    m_param_bind[index].buffer_type = MYSQL_TYPE_STRING;
    m_param_bind[index].buffer = (char *)value.c_str();
    m_param_bind[index].buffer_length = value.size();
}

// 执行更新操作
bool CPrepareStatement::ExecuteUpdate()
{
    if (!m_stmt)
    {
        LOG_ERROR << "no m_stmt";
        return false;
    }

    if (mysql_stmt_bind_param(m_stmt, m_param_bind))
    {
        LOG_ERROR << "mysql_stmt_bind_param failed: " << mysql_stmt_error(m_stmt);
        return false;
    }

    if (mysql_stmt_execute(m_stmt))
    {
        LOG_ERROR << "mysql_stmt_execute failed: " << mysql_stmt_error(m_stmt);
        return false;
    }

    if (mysql_stmt_affected_rows(m_stmt) == 0)
    {
        LOG_ERROR << "ExecuteUpdate have no effect";
        return false;
    }

    return true;
}

// 获取自增ID
uint32_t CPrepareStatement::GetInsertId()
{
    return mysql_stmt_insert_id(m_stmt);
}

/////////////////////
// 数据库连接构造函数
CDBConn::CDBConn(CDBPool *pPool)
{
    m_pDBPool = pPool;
    m_mysql = NULL;
}

// 数据库连接析构函数
CDBConn::~CDBConn()
{
    if (m_mysql)
    {
        mysql_close(m_mysql);
    }
}

// 初始化数据库连接
int CDBConn::Init()
{
    m_mysql = mysql_init(NULL); // 初始化mysql连接
    if (!m_mysql)
    {
        LOG_ERROR << "mysql_init failed";
        return 1;
    }

    bool reconnect = true;
    mysql_options(m_mysql, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(m_mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4"); // 设置字符集

    // 连接数据库
    if (!mysql_real_connect(m_mysql, m_pDBPool->GetDBServerIP(), m_pDBPool->GetUsername(), m_pDBPool->GetPasswrod(),
                            m_pDBPool->GetDBName(), m_pDBPool->GetDBServerPort(), NULL, 0))
    {
        LOG_ERROR << "mysql_real_connect failed: " << mysql_error(m_mysql);
        return 2;
    }

    return 0;
}

// 获取连接池名称
const char *CDBConn::GetPoolName()
{
    return m_pDBPool->GetPoolName();
}

// 执行建表、插入等操作
bool CDBConn::ExecuteCreate(const char *sql_query)
{
    mysql_ping(m_mysql);
    if (mysql_real_query(m_mysql, sql_query, strlen(sql_query)))
    {
        LOG_ERROR << "mysql_real_query failed: " <<  mysql_error(m_mysql) <<  ", sql: start transaction";
        return false;
    }

    return true;
}

// 执行更新操作
bool CDBConn::ExecutePassQuery(const char *sql_query)
{
    mysql_ping(m_mysql);
    if (mysql_real_query(m_mysql, sql_query, strlen(sql_query)))
    {
        LOG_ERROR << "mysql_real_query failed: " <<  mysql_error(m_mysql) << ", sql: start transaction";
        return false;
    }

    return true;
}

// 执行删除操作
bool CDBConn::ExecuteDrop(const char *sql_query)
{
    mysql_ping(m_mysql);
    if (mysql_real_query(m_mysql, sql_query, strlen(sql_query)))
    {
        LOG_ERROR << "mysql_real_query failed: " << mysql_error(m_mysql) << ", sql: start transaction";
        return false;
    }

    return true;
}

// 执行查询操作，返回结果集
CResultSet *CDBConn::ExecuteQuery(const char *sql_query)
{
    mysql_ping(m_mysql);
    row_num = 0;
    if (mysql_real_query(m_mysql, sql_query, strlen(sql_query)))
    {
        LOG_ERROR <<  "mysql_real_query failed: " << mysql_error(m_mysql) << ", sql: %s\n" << sql_query;
        return NULL;
    }
    MYSQL_RES *res = mysql_store_result(m_mysql); // 获取结果集
    if (!res)
    {
        LOG_ERROR <<  "mysql_store_result failed: " << mysql_error(m_mysql);
        return NULL;
    }
    row_num = mysql_num_rows(res);
    LOG_INFO << "row_num: " << row_num;
    CResultSet *result_set = new CResultSet(res);
    return result_set;
}

/*
1.执行成功，则返回受影响的行的数目，如果最近一次查询失败的话，函数返回 -1
2.对于delete,将返回实际删除的行数.
3.对于update,如果更新的列值原值和新值一样,如update tables set col1=10 where id=1;
id=1该条记录原值就是10的话,则返回0。
mysql_affected_rows返回的是实际更新的行数,而不是匹配到的行数。
*/
// 执行更新操作，支持影响行数判断
bool CDBConn::ExecuteUpdate(const char *sql_query, bool care_affected_rows)
{
    mysql_ping(m_mysql);

    if (mysql_real_query(m_mysql, sql_query, strlen(sql_query)))
    {
        LOG_ERROR << "mysql_real_query failed: " << mysql_error(m_mysql) << ", sql: " << sql_query;
        return false;
    }

    if (mysql_affected_rows(m_mysql) > 0)
    {
        return true;
    }
    else
    {
        if (care_affected_rows)
        {
            LOG_ERROR << "mysql_real_query failed: " << mysql_error(m_mysql) << ", sql: " << sql_query;
            return false;
        }
        else
        {
            LOG_WARN << "affected_rows=0, sql: " << sql_query;
            return true;
        }
    }
}

// 开启事务
bool CDBConn::StartTransaction()
{
    mysql_ping(m_mysql);

    if (mysql_real_query(m_mysql, "start transaction\n", 17))
    {
        LOG_ERROR <<  "mysql_real_query failed: " << mysql_error(m_mysql) << ", sql: start transaction";
        return false;
    }

    return true;
}

// 回滚事务
bool CDBConn::Rollback()
{
    mysql_ping(m_mysql);

    if (mysql_real_query(m_mysql, "rollback\n", 8))
    {
        LOG_ERROR <<  "mysql_real_query failed: " << mysql_error(m_mysql) << ", sql: rollback";
        return false;
    }

    return true;
}

// 提交事务
bool CDBConn::Commit()
{
    mysql_ping(m_mysql);

    if (mysql_real_query(m_mysql, "commit\n", 6))
    {
        LOG_ERROR << "mysql_real_query failed: " << mysql_error(m_mysql) << ", sql: commit";
        return false;
    }

    return true;
}

// 获取自增ID
uint32_t CDBConn::GetInsertId()
{
    return (uint32_t)mysql_insert_id(m_mysql);
}

////////////////
// 数据库连接池构造函数
CDBPool::CDBPool(const char *pool_name, const char *db_server_ip, uint16_t db_server_port,
                 const char *username, const char *password, const char *db_name, int max_conn_cnt)
{
    m_pool_name = pool_name;
    m_db_server_ip = db_server_ip;
    m_db_server_port = db_server_port;
    m_username = username;
    m_password = password;
    m_db_name = db_name;
    m_db_max_conn_cnt = max_conn_cnt;	 //
    m_db_cur_conn_cnt = MIN_DB_CONN_CNT; // 最小连接数量
}

// 连接池析构函数，释放所有连接
CDBPool::~CDBPool()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort_request = true;
    m_cond_var.notify_all(); // 通知所有在等待的

    for (list<CDBConn *>::iterator it = m_free_list.begin(); it != m_free_list.end(); it++)
    {
        CDBConn *pConn = *it;
        delete pConn;
    }

    m_free_list.clear();
}

// 初始化连接池，创建最小数量的连接
int CDBPool::Init()
{
    for (int i = 0; i < m_db_cur_conn_cnt; i++)
    {
        CDBConn *pDBConn = new CDBConn(this);
        int ret = pDBConn->Init();
        if (ret)
        {
            delete pDBConn;
            return ret;
        }

        m_free_list.push_back(pDBConn);
    }

    return 0;
}

/*
 *TODO: 增加保护机制，把分配的连接加入另一个队列，这样获取连接时，如果没有空闲连接，
 *TODO: 检查已经分配的连接多久没有返回，如果超过一定时间，则自动收回连接，放在用户忘了调用释放连接的接口
 * timeout_ms默认为 0死等
 * timeout_ms >0 则为等待的时间
 */
// 获取数据库连接
CDBConn *CDBPool::GetDBConn(const int timeout_ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_abort_request)
    {
        LOG_WARN << "have aboort";
        return NULL;
    }

    if (m_free_list.empty()) // 没有空闲连接
    {
        // 检查是否达到最大连接数
        if (m_db_cur_conn_cnt >= m_db_max_conn_cnt) // 达到最大连接数，等待
        {
            // 死等，直到有连接可用或连接池退出
            if (timeout_ms <= 0)
            {
                m_cond_var.wait(lock, [this] 
                {
                    return (!m_free_list.empty()) | m_abort_request;
                });
            }
            else
            {
                // 超时等待
                m_cond_var.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] 
                {
                    return (!m_free_list.empty()) | m_abort_request; 
                });
                if (m_free_list.empty())
                {
                    return NULL;
                }
            }

            if (m_abort_request)
            {
                LOG_WARN << "have abort";
                return NULL;
            }
        }
        else // 还没到最大连接数则新建连接
        {
            CDBConn *pDBConn = new CDBConn(this);
            int ret = pDBConn->Init();
            if (ret)
            {
                LOG_ERROR << "Init DBConnecton failed";
                delete pDBConn;
                return NULL;
            }
            else
            {
                m_free_list.push_back(pDBConn);
                m_db_cur_conn_cnt++;
            }
        }
    }

    CDBConn *pConn = m_free_list.front(); // 获取空闲连接
    m_free_list.pop_front();			  // 从空闲队列移除
 
    return pConn;
}

// 归还数据库连接
void CDBPool::RelDBConn(CDBConn *pConn)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    list<CDBConn *>::iterator it = m_free_list.begin();
    for (; it != m_free_list.end(); it++) // 避免重复归还
    {
        if (*it == pConn)
        {
            break;
        }
    }

    if (it == m_free_list.end())
    {
        m_free_list.push_back(pConn);
        m_cond_var.notify_one(); // 通知等待线程
    }
    else
    {
        LOG_WARN << "RelDBConn failed";// 不再次回收连接
    }
}
// 遍历检测是否超时未归还
// pConn->isTimeout(); // 当前时间 - 被请求的时间
// 强制回收  从m_used_list 放回 m_free_list

/////////////////
// 数据库管理器构造函数
CDBManager::CDBManager()
{
}

// 数据库管理器析构函数
CDBManager::~CDBManager()
{
}

// 获取数据库管理器单例
CDBManager *CDBManager::getInstance()
{
    if (!s_db_manager)
    {
        s_db_manager = new CDBManager();
        if (s_db_manager->Init())
        {
            delete s_db_manager;
            s_db_manager = NULL;
        }
    }

    return s_db_manager;
}
 
// 初始化数据库管理器，读取配置并初始化所有连接池
int CDBManager::Init()
{
    CConfigFileReader config_file("tc_http_server.conf");

    char *db_instances = config_file.GetConfigName("DBInstances");

    if (!db_instances)
    {
        LOG_ERROR << "not configure DBInstances";
        return 1;
    }

    char host[64];
    char port[64];
    char dbname[64];
    char username[64];
    char password[64];
    char maxconncnt[64];
    CStrExplode instances_name(db_instances, ',');

    for (uint32_t i = 0; i < instances_name.GetItemCnt(); i++)
    {
        char *pool_name = instances_name.GetItem(i);
        snprintf(host, 64, "%s_host", pool_name);
        snprintf(port, 64, "%s_port", pool_name);
        snprintf(dbname, 64, "%s_dbname", pool_name);
        snprintf(username, 64, "%s_username", pool_name);
        snprintf(password, 64, "%s_password", pool_name);
        snprintf(maxconncnt, 64, "%s_maxconncnt", pool_name);

        char *db_host = config_file.GetConfigName(host);
        char *str_db_port = config_file.GetConfigName(port);
        char *db_dbname = config_file.GetConfigName(dbname);
        char *db_username = config_file.GetConfigName(username);
        char *db_password = config_file.GetConfigName(password);
        char *str_maxconncnt = config_file.GetConfigName(maxconncnt);

        LOG_INFO << "db_host:" << db_host << ", db_port:" << str_db_port << ", db_dbname:" << db_dbname
                 << ", db_username:" << db_username << ", db_password:" << db_password;

        if (!db_host || !str_db_port || !db_dbname || !db_username || !db_password || !str_maxconncnt)
        {
            LOG_FATAL << "not configure db instance: " << pool_name;
            return 2;
        }

        int db_port = atoi(str_db_port);
        int db_maxconncnt = atoi(str_maxconncnt);
        CDBPool *pDBPool = new CDBPool(pool_name, db_host, db_port, db_username, db_password, db_dbname, db_maxconncnt);
        if (pDBPool->Init())
        {
            LOG_ERROR << "init db instance failed: " << pool_name;
            return 3;
        }
        m_dbpool_map.insert(make_pair(pool_name, pDBPool));
    }

    return 0;
}

// 获取指定连接池的数据库连接
CDBConn *CDBManager::GetDBConn(const char *dbpool_name)
{
    map<string, CDBPool *>::iterator it = m_dbpool_map.find(dbpool_name); // 查找连接池
    if (it == m_dbpool_map.end())
    {
        return NULL;
    }
    else
    {
        return it->second->GetDBConn();
    }
}

// 归还数据库连接
void CDBManager::RelDBConn(CDBConn *pConn)
{
    if (!pConn)
    {
        return;
    }

    map<string, CDBPool *>::iterator it = m_dbpool_map.find(pConn->GetPoolName());
    if (it != m_dbpool_map.end())
    {
        it->second->RelDBConn(pConn);
    }
}