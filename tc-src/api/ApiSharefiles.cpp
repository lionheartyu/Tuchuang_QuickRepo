#include "ApiSharefiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <sys/time.h>
#include "redis_keys.h"
#include "Common.h"

// 获取共享文件总数
int getShareFilesCount(CDBConn *pDBConn, int &count)
{
    int ret = 0;
    string str_sql;

    // 查询共享文件数量
    str_sql = formatString("select count from user_file_count where user='%s'", "xxx_share_xxx_file_xxx_list_xxx_count_xxx");
    CResultSet *pResultSet = pDBConn->ExecuteQuery(str_sql.c_str());
    if (pResultSet && pResultSet->Next())
    {
        // 有记录，返回数量
        count = pResultSet->GetInt("count");
        LOG_INFO << "count: " << count;
        ret = 0;
    }
    else if (!pResultSet)
    { // 查询失败
        LOG_ERROR << str_sql << " 操作失败";
        ret = -1;
    }
    else
    {
        delete pResultSet;
        // 没有记录则初始化为0
        ret = 0;
        count = 0;
        // 插入新记录
        str_sql = formatString("insert into user_file_count (user, count) values('%s', %d)", "xxx_share_xxx_file_xxx_list_xxx_count_xxx", 0);
        if (!pDBConn->ExecuteCreate(str_sql.c_str()))
        {
            LOG_ERROR << str_sql << " 操作失败";
            ret = -1;
        }
    }

    return ret;
}

// 获取共享文件总数（带连接池管理）
int handleGetSharefilesCount(int &count)
{
    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);
    int ret = 0;
    ret = getShareFilesCount(pDBConn, count);

    return ret;
}

// 解析共享文件列表请求json，获取start和count
// 参数示例：{"count": 2, "start": 0, "token": "...", "user": "..."}
int decodeShareFileslistJson(string &str_json, int &start, int &count)
{
    bool res;
    Json::Value root;
    Json::Reader jsonReader;
    res = jsonReader.parse(str_json, root);
    if (!res)
    {
        LOG_ERROR << "parse reg json failed ";
        return -1;
    }

    if (root["start"].isNull())
    {
        LOG_ERROR << "start null\n";
        return -1;
    }
    start = root["start"].asInt();

    if (root["count"].isNull())
    {
        LOG_ERROR << "count null\n";
        return -1;
    }
    count = root["count"].asInt();

    return 0;
}

// 字符串格式化工具函数
template <typename... Args>
std::string formatString(const std::string &format, Args... args)
{
    auto size = std::snprintf(nullptr, 0, format.c_str(), args...) + 1; // 计算所需空间
    std::unique_ptr<char[]> buf(new char[size]);
    std::snprintf(buf.get(), size, format.c_str(), args...);
    return std::string(buf.get(), buf.get() + size - 1); // 去掉末尾的'\0'
}

// 获取共享文件列表（普通/分页）
void handleGetShareFilelist(int start, int count, string &str_json)
{
    int ret = 0;
    string str_sql;
    int total = 0;
    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);
    CResultSet *pResultSet = NULL;
    int file_count = 0;
    Json::Value root, files;

    // 获取共享文件总数
    ret = getShareFilesCount(pDBConn, total);
    if (ret != 0)
    {
        LOG_ERROR << "getShareFilesCount err";
        ret = -1;
        goto END;
    }

    // 查询共享文件详细信息
    str_sql = formatString("select share_file_list.*, file_info.url, file_info.size, file_info.type from file_info, \
        share_file_list where file_info.md5 = share_file_list.md5 limit %d, %d", start, count);
    LOG_INFO << "执行: " << str_sql;
    pResultSet = pDBConn->ExecuteQuery(str_sql.c_str());
    if (pResultSet)
    {
        // 遍历所有的内容
        file_count = 0;
        while (pResultSet->Next())
        {
            Json::Value file;
            file["user"] = pResultSet->GetString("user");
            file["md5"] = pResultSet->GetString("md5");
            file["file_name"] = pResultSet->GetString("file_name");
            file["share_status"] = pResultSet->GetInt("share_status");
            file["pv"] = pResultSet->GetInt("pv");
            file["create_time"] = pResultSet->GetString("create_time");
            file["url"] = pResultSet->GetString("url");
            file["size"] = pResultSet->GetInt("size");
            file["type"] = pResultSet->GetString("type");
            files[file_count] = file;
            file_count++;
        }
        if(file_count > 0)
            root["files"] = files;
        ret = 0;
        delete pResultSet;
    }
    else
    {
        ret = -1;
    }
END:
    if (ret == 0)
    {
        root["code"] = 0;
        root["total"] = total;
        root["count"] = file_count;
    }
    else
    {
        root["code"] = 1;
    }
    str_json = root.toStyledString();
}

// 获取共享文件排行榜（按下载量降序）
void handleGetRankingFilelist(int start, int count, string &str_json)
{
    /*
    a) mysql共享文件数量和redis共享文件数量对比，判断是否相等
    b) 如果不相等，清空redis数据，从mysql中导入数据到redis (mysql和redis交互)
    c) 从redis读取数据，给前端反馈相应信息
    */

    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    int ret2 = 0;
    int total = 0;
    char filename[1024] = {0};
    int sql_num;
    int redis_num;
    int score;
    int end;
    RVALUES value = NULL;
    Json::Value root;
    Json::Value files;
    int file_count = 0;

    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);
    CResultSet *pCResultSet = NULL;

    CacheManager *pCacheManager = CacheManager::getInstance();
    CacheConn *pCacheConn = pCacheManager->GetCacheConn("ranking_list");
    AUTO_REAL_CACHECONN(pCacheManager, pCacheConn);
    
    // 获取共享文件的总数量
    ret = getShareFilesCount(pDBConn, total);
    if (ret != 0)
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }
    //===1、mysql共享文件数量
    sql_num = total;

    //===2、redis共享文件数量
    redis_num = pCacheConn->ZsetZcard(FILE_PUBLIC_ZSET);  // Zcard 命令用于计算集合中元素的数量。
    if (redis_num == -1)
    {
        LOG_ERROR << "ZsetZcard  操作失败";
        ret = -1;
        goto END;
    }

    LOG_INFO << "sql_num: " << sql_num << ", redis_num: " << redis_num;

    //===3、mysql共享文件数量和redis共享文件数量对比，判断是否相等
    if (redis_num != sql_num)
    { //===4、如果不相等，清空redis数据，重新从mysql中导入数据到redis (mysql和redis交互)

        // a) 清空redis有序数据
        pCacheConn->del(FILE_PUBLIC_ZSET);  // 删除集合
        pCacheConn->del(FILE_NAME_HASH);    // 删除hash

        // b) 从mysql中导入数据到redis
        strcpy(sql_cmd, "select md5, file_name, pv from share_file_list order by pv desc");
        LOG_INFO << "执行: " << sql_cmd;

        pCResultSet = pDBConn->ExecuteQuery(sql_cmd);
        if (!pCResultSet)
        {
            LOG_ERROR << sql_cmd << " 操作失败";
            ret = -1;
            goto END;
        }

        // 导入每一条记录到redis
        while (pCResultSet->Next())
        {
            char field[1024] = {0};
            string md5 = pCResultSet->GetString("md5");
            string file_name = pCResultSet->GetString("file_name");
            int pv = pCResultSet->GetInt("pv");
            sprintf(field, "%s%s", md5.c_str(), file_name.c_str()); // 文件唯一标识

            // 增加有序集合成员
            pCacheConn->ZsetAdd(FILE_PUBLIC_ZSET, pv, field);

            // 增加hash记录
            pCacheConn->hset(FILE_NAME_HASH, field, file_name);
        }
    }

    //===5、从redis读取数据，给前端反馈相应信息
    value = (RVALUES)calloc(count, VALUES_ID_SIZE); // 分配空间
    if (value == NULL)
    {
        ret = -1;
        goto END;
    }

    file_count = 0;
    end = start + count - 1; // 结束位置
    // 降序获取有序集合的元素
    ret = pCacheConn->ZsetZrevrange(FILE_PUBLIC_ZSET, start, end, value, file_count);
    if (ret != 0)
    {
        LOG_ERROR << "ZsetZrevrange 操作失败";
        ret = -1;
        goto END;
    }

    // 遍历元素，组装json
    for (int i = 0; i < file_count; ++i)
    {
        Json::Value file;
        // 获取文件名
        ret = pCacheConn->hget(FILE_NAME_HASH, value[i], filename);
        if (ret != 0)
        {
            LOG_ERROR << "hget  操作失败";
            ret = -1;
            goto END;
        }
        file["filename"] = filename;

        // 获取下载量
        score = pCacheConn->ZsetGetScore(FILE_PUBLIC_ZSET, value[i]);
        if (score == -1)
        {
            LOG_ERROR << "ZsetGetScore  操作失败";
            ret = -1;
            goto END;
        }
        file["pv"] = score;
        files[i] = file;
    }
    if(file_count > 0)
        root["files"] = files;

END:
    if (ret == 0)
    {
        root["code"] = 0;
        root["total"] = total;
        root["count"] = file_count;
    }
    else
    {
        root["code"] = 1;
    }
    str_json = root.toStyledString();
}

// 封装共享文件数量查询结果为json
int encodeSharefilesJson(int ret, int total, string &str_json)
{
    Json::Value root;
    root["code"] = ret;
    if (ret == 0)
    {
        root["total"] = total; // 正常返回时写入总数
    }
    Json::FastWriter writer;
    str_json = writer.write(root);
}

// 共享文件接口主流程
int ApiSharefiles(string &url, string &post_data, string &str_json)
{
    // 解析url有没有命令
    // count 获取用户文件个数
    // display 获取用户文件信息，展示到前端
    char cmd[20];
    string user_name;
    string token;
    int ret = 0;
    int start = 0; // 文件起点
    int count = 0; // 文件个数

    LOG_INFO << " post_data: " <<  post_data.c_str();

    // 解析命令，获取cmd参数
    QueryParseKeyValue(url.c_str(), "cmd", cmd, NULL);
    LOG_INFO << "cmd = "<< cmd;

    if (strcmp(cmd, "count") == 0) // 获取共享文件个数
    {
        // 获取共享文件个数
        if (handleGetSharefilesCount(count) < 0)
        { 
            encodeSharefilesJson(1, 0, str_json);
        } 
        else
        {
            encodeSharefilesJson(0, count, str_json);
        }
        return 0;
    }
    else 
    {
        // 获取共享文件信息
        // 普通列表、按下载量升序、降序
        if(decodeShareFileslistJson(post_data, start, count) < 0)
        {
            encodeSharefilesJson(1, 0, str_json);
            return 0;
        }
        if (strcmp(cmd, "normal") == 0)
        {
             handleGetShareFilelist(start, count, str_json);    // 获取共享文件
        }
        else if (strcmp(cmd, "pvdesc") == 0)
        {
            handleGetRankingFilelist(start, count, str_json); // 获取共享文件排行榜
        } 
        else 
        {
            encodeSharefilesJson(1, 0, str_json);
        }
    }
    return 0;
}