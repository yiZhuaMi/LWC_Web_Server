#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

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
#include "locker.h"

#include <sys/uio.h>
#include <sys/sem.h>

class http_conn
{
public:
    static const int FILENAME_LEN = 200;       // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;  // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024; // 写缓冲区的大小
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATCH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, int trig_mode); // 初始化新接受的连接
    void close_conn(bool real_close = true);        // 关闭连接
    void process();                                 // 处理客户请求
    bool read();                                    // 非阻塞读操作
    bool write();                                   // 非阻塞写操作

private:
    void init();                       // 初始化连接
    HTTP_CODE process_read();          // 解析http请求
    bool process_write(HTTP_CODE ret); // 填充http应答

    // 以下一组函数被process_read调用以解析http请求
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();

    // 以下一组函数被process_write调用以填充http应答
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;    // 所有socket上的事件都被注册到同一个epoll内核事件表中，所以设置为静态的(静态成员 所有对象共享)
    static int m_user_count; // 统计用户数量(静态成员 所有对象共享)
    static bool m_et;        // 是否启用边沿触发模式

private:
    int m_sockfd;          // 该http连接的socket
    sockaddr_in m_address; // 该http连接对方的socket地址

    char m_read_buf[READ_BUFFER_SIZE];   // 应用读缓冲区(非内核)
    int m_read_idx;                      // 标识读缓冲区中客户端数据的最后一个字节的下一个位置
    int m_checked_idx;                   // 当前正在分析的字符在读缓冲区的位置
    int m_start_line;                    // 当前正在解析的行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE]; // 应用写缓冲区(非内核)
    int m_write_idx;                     // 写缓冲区中待发送的字节数

    CHECK_STATE m_check_state; // 主状态机当前状态
    METHOD m_method;           // 请求方法

    char m_real_file[FILENAME_LEN]; // 客户请求的目标文件的完整路径，内容为doc_root+m_url，doc_root是网站根目录
    char *m_url;                    // 客户请求的目标文件名
    char *m_version;                // http协议版本号，仅支持1.1
    char *m_host;                   // 主机名
    int m_content_length;           //http请求的消息体的长度
    bool m_linger;                  // http请求是否要求保持连接

    char *m_file_address;    // 客户请求的目标文件被mmap到内存中后的起始位置
    struct stat m_file_stat; // 目标文件的状态。通过其获取文件是否存在、是否为目录、是否可读、文件大小等信息
    struct iovec m_iv[2];    // 因为采用writev集中写来执行写操作，内存区域的数组
    int m_iv_count;          // 被写内存块的数量
};

#endif
