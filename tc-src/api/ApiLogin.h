//用户登录接口，处理用户登录请求和验证

#ifndef _API_LOGIN_H_
#define _API_LOGIN_H_
#include <string>
using std::string;
;
int ApiUserLogin(string &url, string &post_data, string &str_json);
#endif // ! _API_LOGIN_H_
