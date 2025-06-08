//分享文件列表接口，获取所有被分享的文件列表

#ifndef _API_SHAREFILES_H_
#define _API_SHAREFILES_H_
#include <string>
using std::string;
;
int ApiSharefiles(string &url, string &post_data, string &str_json);

#endif