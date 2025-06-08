#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "Common.h"
#include <time.h>
#include "DBPool.h"

// 秒传相关状态码
enum Md5State {
    Md5Ok = 0,         // 秒传成功
    Md5Failed = 1,     // 秒传失败
    Md5TokenFaild = 4, // token验证失败
    Md5FileExit = 5,   // 文件已存在
};

// 解析MD5相关的json，提取用户名、token、md5、文件名
int decodeMd5Json(string &str_json, string &user_name, string &token, string &md5, string &filename)
{
    bool res;
    Json::Value root;
    Json::Reader jsonReader;
    res = jsonReader.parse(str_json, root);
    if (!res)
    {
        LOG_ERROR << "parse md5 json failed ";
        return -1;
    }

    if (root["user"].isNull())
    {
        LOG_ERROR << "user null\n";
        return -1;
    }
    user_name = root["user"].asString();

    if (root["token"].isNull())
    {
        LOG_ERROR << "token null\n";
        return -1;
    }
    token = root["token"].asString();

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

// 封装MD5处理结果为json字符串
int encodeMd5Json(int ret, string &str_json)
{
    Json::Value root;
    root["code"] = ret;
    Json::FastWriter writer;
    str_json = writer.write(root);
    return 0;
}

// 秒传处理主流程
void handleDealMd5(const char *user, const char *md5, const char *filename, string &str_json)
{
    Md5State md5_state = Md5Failed;
    int ret = 0;
    int file_ref_count = 0;
    int user_file_count = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    
    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);

    // 1. 查询 file_info 表，获取该md5文件的引用计数
    sprintf(sql_cmd, "select count from file_info where md5 = '%s'", md5);
    LOG_INFO << "执行: " << sql_cmd;
    file_ref_count = 0;
    ret = GetResultOneCount(pDBConn, sql_cmd, file_ref_count); //执行sql语句
    LOG_INFO << "ret: " << ret << ", file_ref_count: " << file_ref_count;
    if (ret == 0) //有结果, 并且返回 file_info被引用的计数 file_ref_count
    {
        // 2. 检查用户是否已经拥有该文件
        sprintf(sql_cmd, "select * from user_file_list where user = '%s' and md5 = '%s' and file_name = '%s'", user, md5, filename);
        LOG_INFO << "执行: " << sql_cmd;
        ret = CheckwhetherHaveRecord(pDBConn, sql_cmd);  // 检测个人是否有记录
        if (ret == 1) // 已存在
        {
            LOG_WARN << "user: " << user << "->  filename: " << filename << ", md5: " << md5 << "已存在";
            md5_state = Md5FileExit;  // 此用户已经有该文件了，不能重复上传
            goto END;
        }

        // 3. 更新 file_info 的 count 字段 +1
        sprintf(sql_cmd, "update file_info set count = %d where md5 = '%s'", file_ref_count + 1, md5);  
        LOG_INFO << "执行: " << sql_cmd;
        if (!pDBConn->ExecutePassQuery(sql_cmd))
        {
            LOG_ERROR << sql_cmd << "操作失败";
            md5_state = Md5Failed;      // 更新文件引用计数失败
            goto END;
        }

        // 4. 向 user_file_list 插入一条新记录
        struct timeval tv;
        struct tm *ptm;
        char time_str[128];
        gettimeofday(&tv, NULL);
        ptm = localtime(&tv.tv_sec);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", ptm);

        sprintf(sql_cmd, "insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) values ('%s', '%s', '%s', '%s', %d, %d)", user, md5, time_str, filename, 0, 0);
        LOG_INFO << "执行: " << sql_cmd;
        if (!pDBConn->ExecuteCreate(sql_cmd))
        {
            LOG_ERROR << sql_cmd << "操作失败";
            md5_state = Md5Failed;
            // 恢复引用计数
            sprintf(sql_cmd, "update file_info set count = %d where md5 = '%s'", file_ref_count, md5);  
            LOG_INFO << "执行: " << sql_cmd;
            if (!pDBConn->ExecutePassQuery(sql_cmd))
            {
                LOG_ERROR << sql_cmd << "操作失败";
            }
            goto END;
        }

        // 5. 查询用户文件数量
        sprintf(sql_cmd, "select count from user_file_count where user = '%s'", user);
        LOG_INFO << "执行: " << sql_cmd;
        user_file_count = 0;
        ret = GetResultOneCount(pDBConn, sql_cmd, user_file_count);
        if (ret == 1) // 没有记录
        {
            // 6. 插入用户文件数量记录
            sprintf(sql_cmd, " insert into user_file_count (user, count) values('%s', %d)", user, 1);
        }
        else if (ret == 0)
        {
            // 7. 更新用户文件数量 count 字段
            sprintf(sql_cmd, "update user_file_count set count = %d where user = '%s'", user_file_count + 1, user);
        } 
        else
        {
            LOG_ERROR << sql_cmd << " 操作失败";
            md5_state = Md5Failed;  // 如果这里还是失败，还是要恢复文件的引用计数
            // 恢复引用计数
            sprintf(sql_cmd, "update file_info set count = %d where md5 = '%s'", file_ref_count, md5);  
            LOG_INFO << "执行: " << sql_cmd;
            if (!pDBConn->ExecutePassQuery(sql_cmd))
            {
                LOG_ERROR << sql_cmd << "操作失败";
            }
            goto END;
        }
        LOG_INFO << "执行: " << sql_cmd;
        if (pDBConn->ExecutePassQuery(sql_cmd) != 0)
        {
            LOG_ERROR << sql_cmd << " 操作失败";
            md5_state = Md5Failed;  // 如果这里还是失败，还是要恢复文件的引用计数
            // 恢复引用计数
            sprintf(sql_cmd, "update file_info set count = %d where md5 = '%s'", file_ref_count, md5);  
            LOG_INFO << "执行: " << sql_cmd;
            if (!pDBConn->ExecutePassQuery(sql_cmd))
            {
                LOG_ERROR << sql_cmd << "操作失败";
            }
            goto END;
        }
        md5_state = Md5Ok;
    }
    else //没有结果，秒传失败
    {
        LOG_INFO << "秒传失败";
        md5_state = Md5Failed;
        goto END;
    }

END:
    /*
    秒传文件：
        秒传成功：  {"code": 0}
        秒传失败：  {"code":1}
        文件已存在：{"code": 5}
    */
    int code = (int)md5_state;
    encodeMd5Json(code, str_json);
}

// 秒传接口主流程
int ApiMd5(string &url, string &post_data, string &str_json)
{
    UNUSED(url);
    // 解析json中信息
    /*
        * {
        user:xxxx,
        token: xxxx,
        md5:xxx,
        fileName: xxx
        }
    */
    string user;
    string md5;
    string token;
    string filename;
    int ret = 0;
    ret = decodeMd5Json(post_data, user, token, md5, filename); //解析json中信息
    if (ret < 0)
    {
        LOG_ERROR << "decodeMd5Json() err";
        encodeMd5Json((int)Md5Failed, str_json); 
        return 0;
    }

    // 验证登陆token，成返回0，失败-1
    ret = VerifyToken(user, token); //  
    if (ret == 0)
    {   
        handleDealMd5(user.c_str(), md5.c_str(), filename.c_str(), str_json); //秒传处理
        return 0;
    }
    else
    {
        LOG_ERROR << "VerifyToken failed";
        encodeMd5Json(HTTP_RESP_TOKEN_ERR, str_json); // token验证失败错误码
        return 0;
    }
}
