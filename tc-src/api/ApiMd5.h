//文件MD5相关操作（如查重、校验文件唯一性）
//MD5文件通常指的是通过 MD5 算法计算得到的文件摘要（哈希值），用来唯一标识一个文件的内容。
#ifndef _API_MD5_H_
#define _API_MD5_H_
#include <string>
using std::string;
;
int ApiMd5(string &url, string &post_data, string &str_json);
#endif // ! _API_MD5_H_
