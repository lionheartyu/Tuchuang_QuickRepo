#include "ApiDealsharefile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "redis_keys.h"
#include <sys/time.h>
#include "Common.h"

// 解析处理分享文件相关的json，提取用户名、md5、文件名
int decodeDealsharefileJson(string &str_json, string &user_name, string &md5, string &filename)
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

    if (root["user"].isNull())
    {
        LOG_ERROR << "user null\n";
        return -1;
    }
    user_name = root["user"].asString();

    if (root["md5"].isNull())
    {
        LOG_ERROR << "md5 null\n";
        return -1;
    }
    md5 = root["md5"].asString();

    if (root["filename"].isNull())
    {
        LOG_ERROR << "filename null\n";
        return -1;
    }
    filename = root["filename"].asString();

    return 0;
}

// 封装处理结果为json字符串
int encodeDealsharefileJson(int ret, string &str_json)
{
    Json::Value root;
    root["code"] = ret;

    Json::FastWriter writer;
    str_json = writer.write(root);
    return 0;
}

// 文件下载量处理，pv字段+1，并同步到redis
int handlePvFile(string &md5, string &filename)
{
    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    int ret2 = 0;
    char fileid[1024] = {0};
    int pv = 0;

    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);
    CacheManager *pCacheManager = CacheManager::getInstance();
    CacheConn *pCacheConn = pCacheManager->GetCacheConn("ranking_list");
    AUTO_REAL_CACHECONN(pCacheManager, pCacheConn);

    // 文件唯一标识
    sprintf(fileid, "%s%s", md5.c_str(), filename.c_str());

    // 1. 查询当前pv
    sprintf(sql_cmd, "select pv from share_file_list where md5 = '%s' and file_name = '%s'", md5.c_str(), filename.c_str());
    LOG_INFO << "执行: " << sql_cmd;
    CResultSet *pResultSet = pDBConn->ExecuteQuery(sql_cmd);
    if (pResultSet && pResultSet->Next())  
    {
        pv = pResultSet->GetInt("pv");
    }
    else
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 2. 更新pv字段+1
    sprintf(sql_cmd, "update share_file_list set pv = %d where md5 = '%s' and file_name = '%s'", pv + 1, md5.c_str(), filename.c_str());
    LOG_INFO << "执行: " << sql_cmd;
    if (!pDBConn->ExecuteUpdate(sql_cmd, false))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 3. redis同步，zset计数+1或新建
    ret2 = pCacheConn->ZsetExit(FILE_PUBLIC_ZSET, fileid);
    if (ret2 == 1) // 已存在
    {
        ret = pCacheConn->ZsetIncr(FILE_PUBLIC_ZSET, fileid);
        if (ret != 0)
        {
            LOG_ERROR << "ZsetIncr" << " 操作失败";
        }
    }
    else if (ret2 == 0) // 不存在
    {
        pCacheConn->ZsetAdd(FILE_PUBLIC_ZSET, pv + 1, fileid);
        pCacheConn->hset(FILE_NAME_HASH, fileid, filename);
    }
    else // redis操作出错
    {
        ret = -1;
        goto END;
    }

END:
    /*
    下载文件pv字段处理
        成功：{"code":0}
        失败：{"code":1}
    */
    if (ret == 0)
    {
        return HTTP_RESP_OK;
    }
    else
    {
        return HTTP_RESP_FAIL;
    }
}

// 取消分享文件，数据库和redis同步删除
int handleCancelShareFile(string &user_name, string &md5, string &filename)
{
    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    char fileid[1024] = {0};
    int count = 0;
    int ret2;

    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);
    CacheManager *pCacheManager = CacheManager::getInstance();
    CacheConn *pCacheConn = pCacheManager->GetCacheConn("ranking_list");
    AUTO_REAL_CACHECONN(pCacheManager, pCacheConn);

    // 文件唯一标识
    sprintf(fileid, "%s%s", md5.c_str(), filename.c_str());

    // 1. 数据库取消分享
    sprintf(sql_cmd, "update user_file_list set shared_status = 0 where user = '%s' and md5 = '%s' and file_name = '%s'", user_name.c_str(), md5.c_str(), filename.c_str());
    LOG_INFO << "执行: " << sql_cmd;
    if (!pDBConn->ExecuteUpdate(sql_cmd, false))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 2. 查询共享文件数量
    sprintf(sql_cmd, "select count from user_file_count where user = '%s'", "xxx_share_xxx_file_xxx_list_xxx_count_xxx");
    LOG_INFO << "执行: " << sql_cmd;
    count = 0;
    ret2 = GetResultOneCount(pDBConn, sql_cmd, count);
    if (ret2 != 0)
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 3. 更新用户文件数量
    if (count >= 1)
    {
        sprintf(sql_cmd, "update user_file_count set count = %d where user = '%s'", count - 1, "xxx_share_xxx_file_xxx_list_xxx_count_xxx");
        LOG_INFO << "执行: " << sql_cmd;
        if (!pDBConn->ExecutePassQuery(sql_cmd))
        {
            LOG_ERROR << sql_cmd << " 操作失败";
            ret = -1;
            goto END;
        }
    }
    else
    {
        LOG_WARN << "出现异常, count: " << count;
    }

    // 4. 删除share_file_list中的记录
    sprintf(sql_cmd, "delete from share_file_list where user = '%s' and md5 = '%s' and file_name = '%s'", user_name.c_str(), md5.c_str(), filename.c_str());
    LOG_INFO << "执行: " << sql_cmd << ", ret =" << ret;
    if (!pDBConn->ExecuteDrop(sql_cmd))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 5. redis同步删除
    ret = pCacheConn->ZsetZrem(FILE_PUBLIC_ZSET, fileid);
    if (ret != 0)
    {
        LOG_INFO << "执行: ZsetZrem 操作失败";
        goto END;
    }
    ret = pCacheConn->hdel(FILE_NAME_HASH, fileid);
    if (ret != 0)
    {
        LOG_INFO << "执行: hdel 操作失败";
        goto END;
    }

END:
    /*
    取消分享：
        成功：{"code": 0}
        失败：{"code": 1}
    */
    if (ret == 0)
    {
        return (HTTP_RESP_OK);
    }
    else
    {
        return (HTTP_RESP_FAIL);
    }
}

// 转存文件到用户文件列表
// 返回值：0成功，-1转存失败，-2文件已存在
int handleSaveFile(string &user_name, string &md5, string &filename)
{
    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    int ret2 = 0;
    // 当前时间戳
    struct timeval tv;
    struct tm *ptm;
    char time_str[128];
    int count;
    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);

    // 1. 检查用户是否已拥有该文件
    sprintf(sql_cmd, "select * from user_file_list where user = '%s' and md5 = '%s' and file_name = '%s'", user_name.c_str(), md5.c_str(), filename.c_str());
    count = -1;
    ret2 = GetResultOneCount(pDBConn, sql_cmd, count);
    if (ret2 == 2) // 已存在
    {
        LOG_ERROR << "user_name: " << user_name << ", filename: " << filename << ", md5: " << md5 << ", 已存在";
        ret = -2;
        goto END;
    }

    // 2. 查询文件引用计数
    sprintf(sql_cmd, "select count from file_info where md5 = '%s'", md5.c_str());
    count = 0;
    ret2 = GetResultOneCount(pDBConn, sql_cmd, count);
    if (ret2 != 0)
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 3. 更新file_info中的count字段+1
    sprintf(sql_cmd, "update file_info set count = %d where md5 = '%s'", count + 1, md5.c_str());
    if (!pDBConn->ExecuteUpdate(sql_cmd))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 4. user_file_list插入一条新记录
    gettimeofday(&tv, NULL);
    ptm = localtime(&tv.tv_sec);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", ptm);
    sprintf(sql_cmd, "insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) values ('%s', '%s', '%s', '%s', %d, %d)",
            user_name.c_str(), md5.c_str(), time_str, filename.c_str(), 0, 0);
    if (!pDBConn->ExecuteCreate(sql_cmd))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 5. 更新用户文件数量
    sprintf(sql_cmd, "select count from user_file_count where user = '%s'", user_name.c_str());
    count = 0;
    ret2 = GetResultOneCount(pDBConn, sql_cmd, count);
    if (ret2 == 1) // 没有记录
    {
        sprintf(sql_cmd, " insert into user_file_count (user, count) values('%s', %d)", user_name.c_str(), 1);
        if (!pDBConn->ExecuteCreate(sql_cmd))
        {
            LOG_ERROR << sql_cmd << " 操作失败";
            ret = -1;
            goto END;
        }
    }
    else if (ret2 == 0)
    {
        sprintf(sql_cmd, "update user_file_count set count = %d where user = '%s'", count + 1, user_name.c_str());
        if (!pDBConn->ExecuteUpdate(sql_cmd))
        {
            LOG_ERROR << sql_cmd << " 操作失败";
            ret = -1;
            goto END;
        }
    }

END:
    /*
    返回值：0成功，-1转存失败，-2文件已存在
    转存文件：
        成功：{"code":0}
        文件已存在：{"code":5}
        失败：{"code":1}
    */
    if (ret == 0)
    {
        return (HTTP_RESP_OK);
    }
    else if (ret == -1)
    {
        return (HTTP_RESP_FAIL);
    }
    else if (ret == -2)
    {
        return (HTTP_RESP_FILE_EXIST);
    }
    return 0;
}

// 处理分享文件相关的主流程
int ApiDealsharefile(string &url, string &post_data, string &str_json)
{
    char cmd[20];
    string user_name;
    string token;
    string md5;      // 文件md5码
    string filename; // 文件名字
    int ret = 0;

    // 解析url中的cmd参数
    QueryParseKeyValue(url.c_str(), "cmd", cmd, NULL);
    
    // 解析json体
    ret = decodeDealsharefileJson(post_data, user_name, md5, filename);
    LOG_INFO << "cmd: " << cmd << ", user_name:" << user_name << ", md5:" << md5 << ", filename:" << filename;
    if (ret != 0)
    {
        encodeDealsharefileJson(HTTP_RESP_FAIL, str_json);  
        return 0;
    }
    ret = 0;
    if (strcmp(cmd, "cancel") == 0) // 取消分享文件
    {
        ret = handleCancelShareFile(user_name, md5, filename);
    }
    else if (strcmp(cmd, "save") == 0) // 转存文件
    {
        ret = handleSaveFile(user_name, md5, filename);
    }
    else if (strcmp(cmd, "pv") == 0) // 文件下载量处理
    {
        ret = handlePvFile(md5, filename);
    }
    
    // 返回处理结果
    if (ret < 0)
        encodeDealsharefileJson(HTTP_RESP_FAIL, str_json);
    else
        encodeDealsharefileJson(HTTP_RESP_OK, str_json);

    return 0;
}
