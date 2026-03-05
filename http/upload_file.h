#ifndef UPLOADFILE_H
#define UPLOADFILE_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>
#include <dirent.h>    
#include <sys/types.h> 
#include <time.h>
#include <random>
#include <array>
#include <set>
#include <iostream>

#include "../log/log.h"

class UploadFile
{

public:
    UploadFile(char *root, int close_log) : doc_root(root)
    {
        this->m_close_log = close_log;
    };
    UploadFile(const UploadFile &up_file)
    {
        this->doc_root = up_file.doc_root;
    }
    UploadFile &operator=(const UploadFile &up_file)
    {
        this->doc_root = up_file.doc_root;
    }

    UploadFile(UploadFile &&) = default;
    UploadFile &operator=(UploadFile &&) = default;

    ~UploadFile() {}
    // 定义文件长度，读和写缓冲区大小
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048 * 128;
    static const int WRITE_BUFFER_SIZE = 1024 * 32;
    static const size_t MAX_UPLOAD_SIZE = 10 * 1024 * 1024; // 10MB
    // 上传文件模块
    bool save_uploaded_file(const char *filename, string user_name, const char *data, size_t len);
    bool is_valid_path(const char *path);
    bool save_uploaded_chunk(const char *filename, const char *data, size_t len,
                             int chunk_num, int total_chunks);
    void cleanup_chunks();
    void cleanup_chunks(const char *filename, int total_chunks);
    bool is_all_digits(const char *str);
    bool merge_uploaded_file(const char *filename, string user_name, int total_chunks);

private:
    char *doc_root;
    int m_close_log;
};




class URLDecoder {
public:
    /**
     * 解析混合字符串：包含 URL 编码的汉字 + 未编码的英文/数字
     * 例如："Hello%E4%B8%96%E7%95%8C" -> "Hello世界"
     * @param input 混合编码的输入字符串
     * @return 解码后的 UTF-8 字符串
     */
    static std::string decodeMixed(const std::string& input);

private:
    // 判断是否为十六进制字符
    static bool isHexDigit(char c);
    
    // 十六进制字符转数值
    static unsigned char hexToVal(char c);
    
    // 判断是否为可打印 ASCII（保留原样）
    static bool isPrintableASCII(char c);
    
    // 修复不完整的 UTF-8 序列
    static std::string fixUTF8(const std::string& str);
};


#endif