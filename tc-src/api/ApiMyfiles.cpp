#include "ApiMyfiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include "Common.h"
#include "ApiMyfiles.h"
#include "json/json.h"
#include "Logging.h"

// 解析json，获取用户名和token
int decodeCountJson(string &str_json, string &user_name, string &token)
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
    int ret = 0;

    // 用户名
    if (root["user"].isNull())
    {
        LOG_ERROR << "user null\n";
        return -1;
    }
    user_name = root["user"].asString();

    // token
    if (root["token"].isNull())
    {
        LOG_ERROR << "token null\n";
        return -1;
    }
    token = root["token"].asString();

    return ret;
}

// 封装用户文件数量查询结果为json
int encodeCountJson(int ret, int total, string &str_json)
{
    Json::Value root;
    root["code"] = ret;
    if (ret == 0)
    {
        root["total"] = total; // 正常返回时写入文件总数
    }
    Json::FastWriter writer;
    str_json = writer.write(root);
    return 0;
}

// 获取用户文件总数
int getUserFilesCount(string &user_name, int &count)
{
    int ret = 0;
    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);
    string str_sql;

    // 查询用户文件数量
    str_sql = formatString("select `count` from user_file_count where user='%s'", user_name.c_str());
    CResultSet *pResultSet = pDBConn->ExecuteQuery(str_sql.c_str());
    if (pResultSet && pResultSet->Next())
    {
        // 有记录，返回数量
        count = pResultSet->GetInt("count");
        LOG_INFO << "count: " << count;
        ret = 0;
        delete pResultSet;
    }
    else if (!pResultSet)
    { // 查询失败
        LOG_ERROR << str_sql << " 操作失败";
        ret = -1;
    }
    else
    {
        // 没有记录则初始化为0
        ret = 0;
        count = 0;
        // 插入新记录
        str_sql = formatString("insert into user_file_count (user, count) values('%s', %d)", user_name.c_str(), 0);
        if (!pDBConn->ExecuteCreate(str_sql.c_str()))
        {
            LOG_ERROR << str_sql << " 操作失败";
            ret = -1;
        }
    }
    return ret;
}

// 处理用户文件数量查询
int handleUserFilesCount(string &user_name, int &count)
{
    int ret = getUserFilesCount(user_name, count);
    return ret;
}

// 解析文件列表请求json，获取用户名、token、start、count
int decodeFileslistJson(string &str_json, string &user_name, string &token, int &start, int &count)
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
    int ret = 0;

    // 用户名
    if (root["user"].isNull())
    {
        LOG_ERROR << "user null\n";
        return -1;
    }
    user_name = root["user"].asString();

    // token
    if (root["token"].isNull())
    {
        LOG_ERROR << "token null\n";
        return -1;
    }
    token = root["token"].asString();

    // 起始位置
    if (root["start"].isNull())
    {
        LOG_ERROR << "start null\n";
        return -1;
    }
    start = root["start"].asInt();

    // 文件数量
    if (root["count"].isNull())
    {
        LOG_ERROR << "count null\n";
        return -1;
    }
    count = root["count"].asInt();

    return ret;
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

// 获取用户文件列表（支持排序和分页）
int getUserFileList(string &cmd, string &user_name, int &start, int &count, string &str_json)
{
    int ret = 0;

    string str_sql;
    LOG_INFO << "getUserFileList into";

    int total = 0;
    ret = getUserFilesCount(user_name, total);
    if(ret < 0) {
        LOG_ERROR << "getUserFilesCount failed";
        return -1;
    }

    // 根据cmd选择不同的SQL语句
    if (cmd == "normal") // 普通获取
    {
        str_sql = formatString("select user_file_list.*, file_info.url, file_info.size,  file_info.type from file_info, user_file_list where user = '%s' \
            and file_info.md5 = user_file_list.md5 limit %d, %d",
                               user_name.c_str(), start, count);
    }
    else if (cmd == "pvasc") // 按下载量升序
    {
        str_sql = formatString("select user_file_list.*, file_info.url, file_info.size, file_info.type from file_info, \
         user_file_list where user = '%s' and file_info.md5 = user_file_list.md5  order by pv asc limit %d, %d",
                               user_name.c_str(), start, count);
    }
    else if (cmd == "pvdesc") // 按下载量降序
    {
        str_sql = formatString("select user_file_list.*, file_info.url, file_info.size, file_info.type from file_info, \
            user_file_list where user = '%s' and file_info.md5 = user_file_list.md5 order by pv desc limit %d, %d",
                               user_name.c_str(), start, count);
    }
    else
    {
        LOG_ERROR << "unknown cmd: " << cmd;
        return -1;
    }

    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);
    LOG_INFO << "执行: " << str_sql;
    CResultSet *pResultSet = pDBConn->ExecuteQuery(str_sql.c_str());
    if (pResultSet)
    {
        // 遍历所有文件，组装json
        int file_index = 0;
        Json::Value root, files;
        root["code"] = 0;
        while (pResultSet->Next())
        {
            Json::Value file;
            file["user"] = pResultSet->GetString("user");
            file["md5"] = pResultSet->GetString("md5");
            file["create_time"] = pResultSet->GetString("create_time");
            file["file_name"] = pResultSet->GetString("file_name");
            file["share_status"] = pResultSet->GetInt("shared_status");
            file["pv"] = pResultSet->GetInt("pv");
            file["url"] = pResultSet->GetString("url");
            file["size"] = pResultSet->GetInt("size");
            file["type"] = pResultSet->GetString("type");
            files[file_index] = file;
            file_index++;
        }
        root["files"] = files;
        root["count"] = file_index;
        root["total"] = total;
        Json::FastWriter writer;
        str_json = writer.write(root);
        delete pResultSet;
        return 0;
    }
    else
    {
        LOG_ERROR << str_sql << " 操作失败";
        return -1;
    }
}

// 用户文件相关主流程
int ApiMyfiles(string &url, string &post_data, string &str_json)
{
    // 解析url有没有命令

    // count 获取用户文件个数
    // display 获取用户文件信息，展示到前端
    char cmd[20];
    string user_name;
    string token;
    int ret = 0;
    int start = 0; //文件起点
    int count = 0; //文件个数
   

    // 解析url获取cmd参数
    QueryParseKeyValue(url.c_str(), "cmd", cmd, NULL);
    LOG_INFO << "url: " << url << ", cmd = " << cmd;

    if (strcmp(cmd, "count") == 0)
    {
        // 解析json
        if (decodeCountJson(post_data, user_name, token) < 0)
        {
            encodeCountJson(1, 0, str_json);
            LOG_ERROR << "decodeCountJson failed";
            return -1;
        }
        // 验证token
        ret = VerifyToken(user_name, token);
        if (ret == 0)
        {
            // 获取文件数量
            if (handleUserFilesCount(user_name, count) < 0)
            {
                LOG_ERROR << "handleUserFilesCount failed";
                encodeCountJson(1, 0, str_json);
            }
            else
            {
                LOG_INFO << "handleUserFilesCount ok, count:" << count;
                encodeCountJson(0, count, str_json);
            }
        }
        else
        {
            LOG_ERROR << "VerifyToken failed";
            encodeCountJson(1, 0, str_json);
        }
        return 0;
    }
    else 
    {
        // 只允许三种cmd
        if( (strcmp(cmd, "normal") != 0) 
            && (strcmp(cmd, "pvasc") != 0) 
            && (strcmp(cmd, "pvdesc") != 0) )
        {
             LOG_ERROR << "unknow cmd: " << cmd;
            encodeCountJson(1, 0, str_json);
        }
        // 获取用户文件信息
        ret = decodeFileslistJson(post_data, user_name, token, start, count);
        LOG_INFO << "user_name:" << user_name << ", token:" << token << ", start:" << start << ", count:" << count;
        if (ret == 0)
        {
            // 验证token
            ret = VerifyToken(user_name, token);
            if (ret == 0)
            {
                string str_cmd = cmd;
                if (getUserFileList(str_cmd, user_name, start, count, str_json) < 0)
                {
                    LOG_ERROR << "getUserFileList failed";
                    encodeCountJson(1, 0, str_json);
                }
            }
            else
            {
                LOG_ERROR << "VerifyToken failed";
                encodeCountJson(1, 0, str_json);
            }
        }
        else
        {
            LOG_ERROR << "decodeFileslistJson failed";
            encodeCountJson(1, 0, str_json);
        }
    }  

    return 0;
}