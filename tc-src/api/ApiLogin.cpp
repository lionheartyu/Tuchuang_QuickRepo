#include "ApiLogin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include "Common.h"
#include "json/json.h"
#include "Logging.h"
#include "Common.h"
#include "DBPool.h"
#include "CachePool.h"

#define LOGIN_RET_OK 0   // 成功
#define LOGIN_RET_FAIL 1 // 失败

// 解析登录信息，将json字符串解析为用户名和密码
int decodeLoginJson(const std::string &str_json, string &user_name, string &pwd)
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
    // 用户名
    if (root["user"].isNull())
    {
        LOG_ERROR << "user null\n";
        return -1;
    }
    user_name = root["user"].asString();

    // 密码
    if (root["pwd"].isNull())
    {
        LOG_ERROR << "pwd null\n";
        return -1;
    }
    pwd = root["pwd"].asString();

    return 0;
}

// 封装登录结果的json，将登录结果和token写入json字符串
int encodeLoginJson(int ret, string &token, string &str_json)
{
    Json::Value root;
    root["code"] = ret;
    if (ret == 0)
    {
        root["token"] = token; // 正常返回的时候才写入token
    }
    Json::FastWriter writer;
    str_json = writer.write(root);
    return 0;
}

// 格式化字符串工具函数
template <typename... Args>
std::string formatString1(const std::string &format, Args... args)
{
    auto size = std::snprintf(nullptr, 0, format.c_str(), args...) + 1; // 计算所需空间
    std::unique_ptr<char[]> buf(new char[size]);
    std::snprintf(buf.get(), size, format.c_str(), args...);
    return std::string(buf.get(), buf.get() + size - 1); // 去掉末尾的'\0'
}

/**
 * @brief  判断用户登陆情况（校验用户名和密码）
 *
 * @param user_name 用户名
 * @param pwd       密码
 * @returns         成功: 0，失败：-1
 */
int verifyUserPassword(string &user_name, string &pwd)
{
    int ret = 0;
    CDBManager *pDBManager = CDBManager::getInstance();
    CDBConn *pDBConn = pDBManager->GetDBConn("tuchuang_slave");
    AUTO_REAL_DBCONN(pDBManager, pDBConn); // 自动归还数据库连接

    // 查询用户密码
    string strSql = formatString1("select password from user_info where user_name='%s'", user_name.c_str());
    CResultSet *pResultSet = pDBConn->ExecuteQuery(strSql.c_str());
    uint32_t nowtime = time(NULL);
    if (pResultSet && pResultSet->Next())
    {
        // 用户存在，校验密码
        string password = pResultSet->GetString("password");
        LOG_INFO << "mysql-pwd: " << password << ", user-pwd: " << pwd;
        if (pResultSet->GetString("password") == pwd)
            ret = 0;
        else
            ret = -1;
    }
    else
    { // 用户不存在
        ret = -1;
    }

    delete pResultSet;

    return ret;
}

/**
 * @brief  生成token字符串, 保存到redis数据库
 *
 * @param user_name 用户名
 * @param token     生成的token字符串
 * @returns         成功: 0，失败：-1
 */
int setToken(string &user_name, string &token)
{
    int ret = 0;
    CacheManager *pCacheManager = CacheManager::getInstance();
    // 获取Redis连接
    CacheConn *pCacheConn = pCacheManager->GetCacheConn("token");

    // 通过RAII自动归还连接
    AUTO_REAL_CACHECONN(pCacheManager, pCacheConn);

    token = RandomString(32); // 生成32位随机token

    if (pCacheConn)
    {
        // 用户名:token, 86400秒有效（24小时）
        pCacheConn->setex(user_name, 86400, token);
    }
    else
    {
        ret = -1;
    }

    return ret;
}

/**
 * @brief  用户登录主流程
 *
 * @param url       请求url
 * @param post_data 请求体（json字符串）
 * @param str_json  返回的json字符串
 * @returns         成功: 0，失败：-1
 */
int ApiUserLogin(string &url, string &post_data, string &str_json)
{
    UNUSED(url);
    string user_name;
    string pwd;
    string token;
    // 判断数据是否为空
    if (post_data.empty())
    {
        return -1;
    }
    // 解析json
    if (decodeLoginJson(post_data, user_name, pwd) < 0)
    {
        LOG_ERROR << "decodeRegisterJson failed";
        encodeLoginJson(1, token, str_json);
        return -1;
    }

    // 验证账号和密码是否匹配
    if (verifyUserPassword(user_name, pwd) < 0)
    {
        LOG_ERROR << "verifyUserPassword failed";
        encodeLoginJson(1, token, str_json);
        return -1;
    }

    // 生成token
    if (setToken(user_name, token) < 0)
    {
        LOG_ERROR << "setToken failed";
        encodeLoginJson(1, token, str_json);
        return -1;
    }
    // 返回登录成功结果
    encodeLoginJson(0, token, str_json);
    return 0;
}