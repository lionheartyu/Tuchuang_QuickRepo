//文件分享、删除、下载计数等操作（如分享文件、取消分享、下载次数统计）

#ifndef _API_DEALFILE_H_
#define _API_DEALFILE_H_
#include <string>
using std::string;
;
int ApiDealfile(string &url, string &post_data, string &str_json);
int ApiDealfileInit(char *dfs_path_client);

#endif