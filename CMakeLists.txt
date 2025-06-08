cmake_minimum_required(VERSION 2.6)
PROJECT(tc_http_server)

# 添加各目录下的源文件到变量
AUX_SOURCE_DIRECTORY(./ SRC_LIST)
AUX_SOURCE_DIRECTORY(./common COMMON_LIST)
AUX_SOURCE_DIRECTORY(./jsoncpp JSON_LIST)
AUX_SOURCE_DIRECTORY(./mysql MYSQL_LIST)
AUX_SOURCE_DIRECTORY(./redis REDIS_LIST)
AUX_SOURCE_DIRECTORY(./base BASE_LIST)
AUX_SOURCE_DIRECTORY(./api API_LIST)
AUX_SOURCE_DIRECTORY(./log LOG_LIST)

# 设置可执行文件输出路径
SET(EXECUTABLE_OUTPUT_PATH  ./)

# 根据操作系统设置 protobuf 库路径
if(APPLE)
    SET(PB_LIB_DIR ../base/pb/lib/mac)
elseif(UNIX)
    SET(PB_LIB_DIR ../base/pb/lib/linux)
endif()

# 添加编译选项
ADD_DEFINITIONS(
    -g                         # 生成调试信息
    -W                         # 开启所有警告
    -Wall                      # 开启更多警告
    -D_REENTRANT               # 多线程安全
    -D_FILE_OFFSET_BITS=64     # 支持大文件
    -DAC_HAS_INFO              # 日志级别定义
    -DAC_HAS_WARNING
    -DAC_HAS_ERROR
    -DAC_HAS_CRITICAL
    -DTIXML_USE_STL            # TinyXML 使用 STL
    -DAC_HAS_DEBUG
    -DLINUX_DAEMON             # Linux 守护进程
    -std=c++14                 # 使用 C++14 标准
)

# 添加头文件搜索路径
INCLUDE_DIRECTORIES(/usr/include/fastdfs /usr/include/fastcommon /usr/local/include/hiredis /usr/include/mysql)
INCLUDE_DIRECTORIES(./ ./include)
INCLUDE_DIRECTORIES(./jsoncpp)
INCLUDE_DIRECTORIES(./mysql)
INCLUDE_DIRECTORIES(./redis)
INCLUDE_DIRECTORIES(./base)
INCLUDE_DIRECTORIES(./api)
INCLUDE_DIRECTORIES(./log)
include_directories() # 保证 include 目录被包含

# 生成可执行文件，包含所有源文件
# 注意：ADD_EXECUTABLE 必须在 TARGET_LINK_LIBRARIES 前面
ADD_EXECUTABLE(tc_http_server  ${BASE_LIST} ${API_LIST} ${LOG_LIST}
        ${MYSQL_LIST} ${REDIS_LIST} ${JSON_LIST} ${SRC_LIST} ${COMMON_LIST})

# 链接所需的第三方库
TARGET_LINK_LIBRARIES(tc_http_server
    pthread         # 多线程库
    fdfsclient      # FastDFS 客户端库
    fastcommon      # FastDFS 公共库
    hiredis         # Redis 客户端库
    m               # 数学库
    mysqlclient     # MySQL 客户端库
)
