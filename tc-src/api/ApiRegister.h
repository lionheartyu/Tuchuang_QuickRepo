//用户注册接口，处理新用户注册请求

#ifndef _API_REGISTER_H_
#define _API_REGISTER_H_
#include <string>
using std::string;
int ApiRegisterUser(string &url, string &post_data, string &str_json);
#endif