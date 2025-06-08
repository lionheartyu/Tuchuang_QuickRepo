#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "Common.h"
#include <time.h>

// 解析分享图片请求的json，提取用户名、token、md5、文件名
int decodeSharePictureJson(string &str_json, string &user_name, string &token, string &md5, string &filename)
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

    if (root["token"].isNull())
    {
        LOG_ERROR << "token null\n";
        return -1;
    }
    token = root["token"].asString();

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

// 封装分享图片结果为json
int encodeSharePictureJson(int ret, string urlmd5, string &str_json)
{
    Json::Value root;
    root["code"] = ret;
    if (HTTP_RESP_OK == ret)
        root["urlmd5"] = urlmd5;
    Json::FastWriter writer;
    str_json = writer.write(root);
    return 0;
}

// 解析图片列表请求json，提取用户名、token、start、count
int decodePictureListJson(string &str_json, string &user_name, string &token, int &start, int &count)
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

    if (root["token"].isNull())
    {
        LOG_ERROR << "token null\n";
        return -1;
    }
    token = root["token"].asString();

    if (root["user"].isNull())
    {
        LOG_ERROR << "user null\n";
        return -1;
    }
    user_name = root["user"].asString();

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

// 解析取消分享图片请求json，提取用户名和urlmd5
int decodeCancelPictureJson(string &str_json, string &user_name, string &urlmd5)
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

    if (root["urlmd5"].isNull())
    {
        LOG_ERROR << "urlmd5 null\n";
        return -1;
    }
    urlmd5 = root["urlmd5"].asString();

    return 0;
}

// 封装取消分享图片结果为json
int encodeCancelPictureJson(int ret, string &str_json)
{
    Json::Value root;
    root["code"] = ret;
    Json::FastWriter writer;
    str_json = writer.write(root);
    return 0;
}

// 解析浏览图片请求json，提取urlmd5
int decodeBrowsePictureJson(string &str_json, string &urlmd5)
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

    if (root["urlmd5"].isNull())
    {
        LOG_ERROR << "urlmd5 null\n";
        return -1;
    }
    urlmd5 = root["urlmd5"].asString();

    return 0;
}

// 封装浏览图片结果为json
int encodeBrowselPictureJson(int ret, int pv, string url, string user, string time, string &str_json)
{
    Json::Value root;
    root["code"] = ret;
    if (ret == 0)
    {
        root["pv"] = pv;
        root["url"] = url;
        root["user"] = user;
        root["time"] = time;
    }
    Json::FastWriter writer;
    str_json = writer.write(root);
    return 0;
}

// 获取用户分享图片总数
int getSharePicturesCount(CDBConn *pDBConn, const char *user, int &count)
{
    char sql_cmd[SQL_MAX_LEN] = {0};
    int ret = 0;

    sprintf(sql_cmd, "select count from user_file_count where user='%s%s'", user, "_share_picture_list_count");
    CResultSet *pResultSet = pDBConn->ExecuteQuery(sql_cmd);
    if (pResultSet && pResultSet->Next())
    {
        // 存在则返回
        count = pResultSet->GetInt("count");
        LOG_INFO << "count: " << count;
        ret = 0;
        delete pResultSet;
    }
    else if (!pResultSet)
    { // 操作失败
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
    }
    else
    {
        delete pResultSet;
        // 没有记录则初始化记录数量为0
        ret = 0;
        count = 0;
        // 创建
        sprintf(sql_cmd, "insert into user_file_count (user, count) values('%s%s', %d)", user, "_share_picture_list_count", 0);
        if (!pDBConn->ExecuteCreate(sql_cmd))
        { // 操作失败
            LOG_ERROR << sql_cmd << " 操作失败";
            ret = -1;
        }
    }

    return ret;
}

// 获取用户分享图片列表
void handleGetSharePicturesList(CDBConn *pDBConn, const char *user, int start, int count, string &str_json)
{
    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    CResultSet *pResultSet;
    int total = 0;
    int file_count = 0;
    Json::Value root;

    total = 0;
    ret = getSharePicturesCount(pDBConn, user, total);
    if (ret != 0)
    {
        LOG_ERROR << "getSharePicturesCount failed";
        ret = -1;
        goto END;
    }
    if (total == 0)
    {
        LOG_INFO << "getSharePicturesCount count = 0";
        ret = 0;
        goto END;
    }

    // 查询分享图片详细信息
    sprintf(sql_cmd, "select share_picture_list.user, share_picture_list.filemd5, share_picture_list.file_name,share_picture_list.urlmd5, share_picture_list.pv, \
        share_picture_list.create_time, file_info.size from file_info, share_picture_list where share_picture_list.user = '%s' and  \
        file_info.md5 = share_picture_list.filemd5 limit %d, %d",
            user, start, count);
    LOG_INFO << "执行: " << sql_cmd;
    pResultSet = pDBConn->ExecuteQuery(sql_cmd);
    if (pResultSet)
    {
        // 遍历所有的内容
        Json::Value files;

        while (pResultSet->Next())
        {
            Json::Value file;
            file["user"] = pResultSet->GetString("user");
            file["filemd5"] = pResultSet->GetString("filemd5");
            file["file_name"] = pResultSet->GetString("file_name");
            file["urlmd5"] = pResultSet->GetString("urlmd5");
            file["pv"] = pResultSet->GetInt("pv");
            file["create_time"] = pResultSet->GetString("create_time");
            file["size"] = pResultSet->GetInt("size");
            files[file_count] = file;
            file_count++;
        }
        if(file_count >0 )
            root["files"] = files;

        ret = 0;
        delete pResultSet;
    }
    else
    {
        ret = -1;
    }

END:
    if (ret != 0)
    {
        Json::Value root;
        root["code"] = 1;
    }
    else
    {
        root["code"] = 0;
        root["count"] = file_count;
        root["total"] = total;
    }
    str_json = root.toStyledString();
    LOG_INFO << "str_json:" << str_json;

    return;
}

// 取消分享图片
void handleCancelSharePicture(CDBConn *pDBConn, const char *user, const char *urlmd5, string &str_json)
{
    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    int line = 0;
    int count = 0;
    int ret2;
    CResultSet *pResultSet;

    // 查询是否存在该分享图片
    sprintf(sql_cmd, "select * from share_picture_list where user = '%s' and urlmd5 = '%s'", user, urlmd5);
    LOG_INFO << "执行: " << sql_cmd;
    pResultSet = pDBConn->ExecuteQuery(sql_cmd);
    if (!pResultSet)
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }
    delete pResultSet;
    line = pDBConn->GetRowNum();
    LOG_ERROR << "GetRowNum = " << line;
    if (line == 0)
    {
        ret = 0;
        goto END;
    }
    // 查询共享文件数量
    sprintf(sql_cmd, "select count from user_file_count where user = '%s%s'", user, "_share_picture_list_count");
    count = 0;
    ret2 = GetResultOneCount(pDBConn, sql_cmd, count); //执行sql语句
    if (ret2 != 0)
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 更新用户文件数量count字段
    if (count > 0)
    {
        sprintf(sql_cmd, "update user_file_count set count = %d where user = '%s%s'", count - 1, user, "_share_picture_list_count");
        LOG_INFO << "执行: " << sql_cmd;
        if (!pDBConn->ExecutePassQuery(sql_cmd))
        {
            LOG_ERROR << sql_cmd << " 操作失败";
        }
    }

    // 删除在共享列表的数据
    sprintf(sql_cmd, "delete from share_picture_list where user = '%s' and urlmd5 = '%s'", user, urlmd5);
    LOG_INFO << "执行: " << sql_cmd;
    if (!pDBConn->ExecutePassQuery(sql_cmd))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
    }
END:
    /*
    取消分享：
        成功：{"code": 0}
        失败：{"code": 1}
    */
    if (0 == ret)
        encodeCancelPictureJson(HTTP_RESP_OK, str_json);
    else
        encodeCancelPictureJson(HTTP_RESP_FAIL, str_json);
}

// 分享图片主流程
int handleSharePicture(CDBConn *pDBConn, const char *user, const char *filemd5, const char *file_name, string &str_json)
{
    char key[5] = {0};
    int count = 0;
    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    int ret2 = 0;
    char create_time[TIME_STRING_LEN];
    string urlmd5;
    urlmd5 = RandomString(32); // 生成urlmd5

    LOG_INFO << "urlmd5: " << urlmd5;

    // 生成urlmd5，生成提取码
    time_t now;
    now = time(NULL);
    strftime(create_time, TIME_STRING_LEN - 1, "%Y-%m-%d %H:%M:%S", localtime(&now));

    // 插入share_picture_list
    sprintf(sql_cmd, "insert into share_picture_list (user, filemd5, file_name, urlmd5, `key`, pv, create_time) values ('%s', '%s', '%s', '%s', '%s', %d, '%s')",
            user, filemd5, file_name, urlmd5.c_str(), key, 0, create_time);
    LOG_INFO << "执行: " << sql_cmd;
    if (!pDBConn->ExecuteCreate(sql_cmd))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }

    // 查询共享图片数量
    sprintf(sql_cmd, "select count from user_file_count where user = '%s%s'", user, "_share_picture_list_count");
    LOG_INFO << "执行: " << sql_cmd;

    count = 0;
    ret2 = GetResultOneCount(pDBConn, sql_cmd, count); //执行sql语句
    if (ret2 == 1) //没有记录
    {
        //插入记录
        sprintf(sql_cmd, "insert into user_file_count (user, count) values('%s%s', %d)", user, "_share_picture_list_count", 1);
    }
    else if (ret2 == 0)
    {
        //更新用户文件数量count字段
        sprintf(sql_cmd, "update user_file_count set count = %d where user = '%s%s'", count + 1, user, "_share_picture_list_count");
    }

    if (!pDBConn->ExecutePassQuery(sql_cmd))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }
    ret = 0;
END:

    // 返回urlmd5
    if (ret == 0)
    {
        encodeSharePictureJson(HTTP_RESP_OK, urlmd5, str_json);
    }
    else
    {
        encodeSharePictureJson(HTTP_RESP_FAIL, urlmd5, str_json);
    }

    return ret;
}

// 浏览图片主流程
int handleBrowsePicture(CDBConn *pDBConn, const char *urlmd5, string &str_json)
{
    int ret = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};
    string picture_url;
    string file_name;
    string user;
    string filemd5;
    string create_time;
    int pv = 0;

    CResultSet *pResultSet = NULL;

    LOG_INFO << "urlmd5: " << urlmd5;
    // 查询分享图片信息
    sprintf(sql_cmd, "select user, filemd5, file_name, pv, create_time from share_picture_list where urlmd5 = '%s'", urlmd5);
    LOG_DEBUG << "执行: " << sql_cmd;
    pResultSet = pDBConn->ExecuteQuery(sql_cmd);
    if (pResultSet && pResultSet->Next())
    {
        user = pResultSet->GetString("user");
        filemd5 = pResultSet->GetString("filemd5");
        file_name = pResultSet->GetString("file_name");
        pv = pResultSet->GetInt("pv");
        create_time = pResultSet->GetString("create_time");
        delete pResultSet;
    }
    else
    {
        if (pResultSet)
            delete pResultSet;
        ret = -1;
        goto END;
    }

    // 通过文件的MD5查找对应的url地址
    sprintf(sql_cmd, "select url from file_info where md5 ='%s'", filemd5.c_str());
    LOG_INFO << "执行: " << sql_cmd;
    pResultSet = pDBConn->ExecuteQuery(sql_cmd);
    if (pResultSet && pResultSet->Next())
    {
        picture_url = pResultSet->GetString("url");
        delete pResultSet;
    }
    else
    {
        if (pResultSet)
            delete pResultSet;
        ret = -1;
        goto END;
    }

    // 更新浏览次数
    sprintf(sql_cmd, "update share_picture_list set pv = %d where urlmd5 = '%s'", pv + 1, urlmd5);
    LOG_DEBUG << "执行: " << sql_cmd;
    if (!pDBConn->ExecuteUpdate(sql_cmd))
    {
        LOG_ERROR << sql_cmd << " 操作失败";
        ret = -1;
        goto END;
    }
    ret = 0;
END:
    // 返回图片信息
    if (ret == 0)
    {
        encodeBrowselPictureJson(HTTP_RESP_OK, pv, picture_url, user, create_time, str_json);
    }
    else
    {
        encodeBrowselPictureJson(HTTP_RESP_FAIL, pv, picture_url, user, create_time, str_json);
    }
}

// 图床分享接口主流程
int ApiSharepicture(string &url, string &post_data, string &str_json)
{
    char cmd[20];
    string user_name; //用户名
    string md5;       //文件md5码
    string urlmd5;
    string filename; //文件名字
    string token;
    int ret = 0;
    // 解析命令
    QueryParseKeyValue(url.c_str(), "cmd", cmd, NULL);
    LOG_INFO << "cmd = " << cmd;

    // 获取数据库连接
    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn);

    if (strcmp(cmd, "share") == 0) //分享文件
    {
        ret = decodeSharePictureJson(post_data, user_name, token, md5, filename); //解析json信息
        if (ret == 0)
        {
            handleSharePicture(pDBConn, user_name.c_str(), md5.c_str(), filename.c_str(), str_json);
        }
        else
        {
            // 回复请求格式错误
            encodeSharePictureJson(HTTP_RESP_FAIL, urlmd5, str_json);
        }
    }
    else if (strcmp(cmd, "normal") == 0) //获取图片列表
    {
        int start = 0;
        int count = 0;
        ret = decodePictureListJson(post_data, user_name, token, start, count);
        if (ret == 0)
        {
            handleGetSharePicturesList(pDBConn, user_name.c_str(), start, count, str_json);
        }
        else
        {
            // 回复请求格式错误
            encodeSharePictureJson(HTTP_RESP_FAIL, urlmd5, str_json);
        }
    }
    else if (strcmp(cmd, "cancel") == 0) //取消分享文件
    {
        ret = decodeCancelPictureJson(post_data, user_name, urlmd5);
        if (ret == 0)
        {
            handleCancelSharePicture(pDBConn, user_name.c_str(), urlmd5.c_str(), str_json);
        }
        else
        {
            // 回复请求格式错误
            encodeCancelPictureJson(1, str_json);
        }
    }
    else if (strcmp(cmd, "browse") == 0) //浏览图片
    {
        ret = decodeBrowsePictureJson(post_data, urlmd5);
        LOG_INFO << "post_data: " << post_data << ", urlmd5: " << urlmd5;
        if (ret == 0)
        {
            handleBrowsePicture(pDBConn, urlmd5.c_str(), str_json);
        }
        else
        {
            // 回复请求格式错误
            encodeSharePictureJson(HTTP_RESP_FAIL, urlmd5, str_json);
        }
    } 
    else
    {
        // 回复请求格式错误
        encodeSharePictureJson(HTTP_RESP_FAIL, urlmd5, str_json);
    }

    return 0;
}
