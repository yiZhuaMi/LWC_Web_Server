#include "http_conn.h"

const char *ok_200_title = "200 OK";
const char *error_400_title = "400 Bad Request";
const char *error_400_form = "400 Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "403 Forbidden";
const char *error_403_form = "403 You do not have permission to get file from this server.\n";
const char *error_404_title = "404 Not Found";
const char *error_404_form = "404 The requested file was not found on this server.\n";
const char *error_500_title = "500 Internal Error";
const char *error_500_form = "500 There was an unusual problem serving the requested file.\n";
const char *doc_root = "../doc_root";

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 往内核事件表注册fd上的事件
void addfd(int epollfd, int fd, bool one_shot, int trig_mode)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    // 外部传入,可能是设置listenfd的mode,也可能是connfd的mode
    if (trig_mode == 1)
    {
        event.events |= EPOLLET;
    }
    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 删除fd上的所有注册事件
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT 解除对该fd的独占 确保下一次能被触发
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    // 只有connfd会调用modfd,直接读m_et就好,无需传参
    if (http_conn::m_et)
    {
        event.events |= EPOLLET;
    }
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("关闭连接\n");
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in &addr, int trig_mode)
{
    m_sockfd = sockfd;
    m_address = addr;
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true, trig_mode);
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    // m_checked_idx指向读缓冲区中当前正在分析的字节
    // m_read_idx指向读缓冲区中客户端数据的尾部的下一个字节
    // 读缓冲区中 0～m_checked_idx 个字节都已经分析完毕
    // 第 m_checked_idx～(m_read_idx-1) 个字节由下面循环逐个分析
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx]; // 获取当前分析的字节
        if (temp == '\r')                 // 是回车符 则有可能读到一个完整的行
        {
            if ((m_checked_idx + 1) == m_read_idx) // temp是最后一个但还不是\n 读完了所有数据还不到一行
            {
                return LINE_OPEN; // 行数据尚不完整
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n') // 读到了完整的一行
            {
                m_read_buf[m_checked_idx++] = '\0'; // 替换换行符
                m_read_buf[m_checked_idx++] = '\0'; // ???
                return LINE_OK;
            }
            // \r后面还有但不是\n
            return LINE_BAD;
        }
        else if (temp == '\n') // 是换行符 则有可能读到一个完整的行
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) // 前一个是回车符
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD; // 行出错
        }
    }

    return LINE_OPEN; // 最后一个不是换行或回车符
}

// 非阻塞读操作
bool http_conn::read()
{
    // 开始读的字节序号=客户数据的结尾后一个>=读缓冲区了 --> 越界错误？
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read = 0;

    if (0)
    {
        // LT读
        printf("LT读\n");
        bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0)
        {
            return false;
        }
    }
    else
    {
        // ET写 确保把socket读缓冲区中的所有数据读出
        while (true)
        {
            // recv是否阻塞是根据socket是否阻塞，这里是非阻塞
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) // 读失败
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) // 缓冲区空 全部被读完了
                {
                    break; //跳出循环 return true 添加任务 等待下一次可读事件 
                }
                return false; // 其他失败原因
            }
            else if (bytes_read == 0) // 连接被对方关闭
            {
                return false;
            }
            m_read_idx += bytes_read; // 加上这次读取的字节数
        }
    }
    return true;// 直到把缓冲区读空 才返回
}

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 在text中检索" \t"(空格)，返回第一个匹配的位置，未找到则返回NULL
    // 在源字符串（s1）中找出最先含有搜索字符串（s2）中任一字符的位置并返回，若找不到则返回空指针。
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        printf("m_url为空\n");
        return BAD_REQUEST;
    }
    *m_url++ = '\0'; // 截断字符串

    char *method = text;
    if (strcasecmp(method, "GET") == 0) // 仅支持GET方法
    {
        m_method = GET;
    }
    else
    {
        printf("不支持%s方法\n", method);
        return BAD_REQUEST;
    }

    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
    // 若strcspn()返回的数值为n, 则代表字符串str1开头连续有n个字符都不含字符串str2内的字符.
    m_url += strspn(m_url, " \t"); // 跳过空格
    // 在源字符串（s1）中找出最先含有搜索字符串（s2）中任一字符的位置并返回，若找不到则返回空指针。
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        printf("m_version为空\n");
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        printf("Only supports HTTP/1.1 and your request is %s\n", m_version);
        return BAD_REQUEST;
    }

    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
    }

    if (!m_url || m_url[0] != '/')
    {
        printf("! m_url || m_url[ 0 ] != '/'\n");
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; // 状态转移 从解析请求行变为解析头部
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        if (m_method == HEAD)
        {
            return GET_REQUEST;
        }
        // 请求有消息体，则还需读取CHECK_STATE_CONTENT字节的消息体，状态机转到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则已经得到了完整的请求
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        // printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;
}

// 没有真正的解析http请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

// 主状态机 分析http请求的入口函数
http_conn::HTTP_CODE http_conn::process_read()
{
    // 记录当前行的读取状态
    LINE_STATUS line_status = LINE_OK;
    // 记录http请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // (正在分析消息主体 && 读到了完整行) || (正在分析请求行或者头部，那就使用从状态机读一行看是否完整)
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
        || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();            // 获取当前要读的行的起始位置
        m_start_line = m_checked_idx; // 记录下一行的起始位置
        printf("got 1 http line: %s\n", text);

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE: // 分析请求行
        {
            ret = parse_request_line(text);
            // printf("m_url:%s\n",m_url);
            if (ret == BAD_REQUEST) // 请求不完整
            {
                printf("BAD_REQUEST:request line 不完整\n");
                return BAD_REQUEST;
            }
            break; // 请求行解析完成 开始解析头部字段
        }
        case CHECK_STATE_HEADER: // 分析头部字段
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST) // 请求不完整
            {
                printf("BAD_REQUEST:header 不完整\n");
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST) // 获得了完整的客户请求
            {
                // content为空的情况
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT: // 分析消息主体
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST) // 获得了完整的客户请求
            {
                return do_request();
            }
            line_status = LINE_OPEN; // 行数据尚不完整
            break;
        }
        default:
        {
            printf("INTERNAL_ERROR\n");
            return INTERNAL_ERROR;
        }
        }
    }

    printf("NO_REQUEST\n");
    return NO_REQUEST;
}

// 当得到一个完整、正确的http请求时，分析目标文件的属性
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // 将m_url复制到doc_root后面
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // 取得m_real_file文件属性，文件属性存储在结构体m_file_stat里
    // printf("m_real_file:%s\n",m_real_file);
    if (stat(m_real_file, &m_file_stat) < 0) // 获取失败返回-1 目标文件不存在
    {
        printf("NO_RESOURCE\n");
        return NO_RESOURCE;
    }

    if (!(m_file_stat.st_mode & S_IROTH)) // ！目标文件对所有用户可读
    {
        printf("FORBIDDEN_REQUEST\n");
        return FORBIDDEN_REQUEST;
    }

    if (S_ISDIR(m_file_stat.st_mode)) // 目标文件是目录
    {
        printf("BAD_REQUEST:目标文件是目录\n");
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);
    // 使用mmap将目标文件映射到内存地址m_file_address处
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    // 告诉调用者获取文件成功
    printf("FILE_REQUEST:成功获取目标资源\n");
    return FILE_REQUEST;
}

// 释放内存
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size); // 内存起始地址，长度
        m_file_address = 0;
    }
}

// 写http响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx; // 应用写缓冲区中待发送的字节数
    // 由于没有数据要写
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 监听可读事件 解除对该fd的独占
        init();                              // 重置http_conn状态
        return true;                         // 保留http_conn连接
    }

    while (1)
    {
        // 集中写：多块分散内存的数据一并写入文件描述符对应的内核写缓冲区 iovec描述一块内存区域 成功则返回写入fd的字节数
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) // 写失败
        {
            // 如果TCP写缓冲区没有空间，则等待下一轮EPOLLOUT事件（内核缓冲区有空间写）。
            // 虽然在此期间服务器无法立即接受到同一个客户端的下一个请求，但这可以保证连接的完整性。
            if (errno == EAGAIN) // 非阻塞写 写缓冲区满
            {
                printf("写缓冲区满 请重试\n");
                modfd(m_epollfd, m_sockfd, EPOLLOUT); // 解除对该fd的独占 然后等待下一次可写事件
                return true;                          // 保留http_conn连接
            }
            printf("写出错\n");
            unmap();      // 释放客户请求文件的内存
            return false; // 关闭http_conn
        }
        static int resp_times = 0;
        printf("写响应成功 %d 次\n", ++resp_times);

        // 待发送的减去temp
        bytes_to_send -= temp;
        // 已发送的加上temp
        bytes_have_send += temp;
        
        // 一次只写一半到内核缓冲区，如何确保一次性发完一整个response？？？
        // if (bytes_to_send <= bytes_have_send)
        // TODO:测试
        if (bytes_to_send <= 0)
        {
            unmap();      // 释放客户请求文件的内存
            // 取消监听可写 否则由于写缓冲区可写（未满）则立即触发EPOLLOUT
            if (m_linger) // http请求要求保持连接
            {
                init();                              // 重置http_conn状态
                modfd(m_epollfd, m_sockfd, EPOLLIN); // 监听可读事件 取消监听可写 解除对该fd的独占
                return true;                         // 保留http_conn连接
            }
            else
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN); // 监听可读事件 取消监听可写 解除对该fd的独占
                return false;                        // 会关闭http_conn
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 构造响应 响应内容不在同一块内存 所以主线程中用集中写writev写响应
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR: // 服务器内部错误
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    }
    case BAD_REQUEST: // 客户请求有语法错误
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    }
    case NO_RESOURCE: // 资源没找到
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST: // 客户对资源没有访问权限
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    case FILE_REQUEST: // 成功获取文件资源
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf; // 写缓冲区
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address; // 请求的目标文件
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        }
        else // 目标文件大小为0
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
            {
                return false;
            }
        }
    }
    default:
    {
        return false;
    }
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) // 请求不完整 但可以继续读
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 监听可读事件 解除对该fd的独占
        return;
    }

    // 否则成功获取资源或者出错 并根据read_ret构造响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) // 构造响应出错
    {
        close_conn(); // 从内核事件表移除m_sockfd 并用户数量-1
    }
    // 够造响应成功 等待内核缓冲区有空间可写
    modfd(m_epollfd, m_sockfd, EPOLLOUT); // 监听可写事件 解除对该fd的独占
}
