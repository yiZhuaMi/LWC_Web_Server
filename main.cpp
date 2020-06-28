#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"

// #define LT// 电平触发
#define ET// 边沿触发

#ifdef ET
    bool http_conn::m_et = true;
#endif

#ifdef LT
    bool http_conn::m_et = false;
#endif

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

// 内核事件表描述符
static int epollfd = 0;

// 设定定时器相关参数
static int pipefd[2];           // 信号处理函数与主循环通信的管道
static sort_timer_lst timer_lst; // 升序链表定时器

// 信号处理函数
void sig_handler(int sig)
{
    // 保留原来的errno，在函数最后恢复，以保证函数的可重入性
    int save_errno = errno;
    int msg = sig;
    // 将信号值写入管道已通知主循环
    int ret = send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号处理函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler; // 设置信号处理函数
    if (restart)
    {
        sa.sa_flags |= SA_RESTART; // 重新调用被该信号终止的系统调用
    }
    sigfillset(&sa.sa_mask); // 在信号集中设置所有的信号？？？
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，实际上就是调用tick函数
void timer_handler()
{
    printf("连接数量:%d\n",timer_lst.get_list_size());
    // 定时器链表有连接才会tick
    timer_lst.tick();
    // 由于alarm只会引起一次SIGALARM信号，所以需要重新定时，以不断触发SIGALARM信号
    alarm(TIMESLOT);
}

// 定时器回调函数，删除非活动连接socket上的注册事件并关闭之
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);// 关闭socket连接
    http_conn::m_user_count--;// 静态成员 所有对象共享 用户数量减1
    printf("close fd %d\n",user_data->sockfd);
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
    if (argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    // 忽略SIGPIPE信号
    addsig(SIGPIPE, SIG_IGN);

    // 创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        // 初始化线程池，子线程用信号量来同步任务的竞争
        pool = new threadpool<http_conn>;
    }
    catch (...)
    {
        return 1;
    }

    // 预先为每个可能的客户连接分配一个http_conn对象
    http_conn *users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    // IPv4 TCP 0:默认协议
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    // 失败返回-1
    assert(listenfd >= 0);

    // l_onoff != 0 l_linger = 0
    // close()立刻返回，但不会发送未发送完成的数据，而是通过一个REST包强制关闭(没有四次挥手)socket描述符，即强制退出。
    // 这里{1,0}强制关闭使得客户读出错 errno=104
    // struct linger tmp = { 0, 0 };
    // 设置socket选项
    // setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    // 强制使用被处于TIME_WAIT状态的连接占用的socket地址
    int reuse = 1;
    // 设置socket选项
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int ret = 0;
    // 专用socket地址IPv4
    struct sockaddr_in address;
    // 将字符串s的前n个字节置为0
    bzero(&address, sizeof(address));
    // 地址族设为IPv4
    address.sin_family = AF_INET;
    // 将字符串表示的IP地址（点分十进制）转换为网络字节序整数表示的IP地址
    inet_pton(AF_INET, ip, &address.sin_addr);
    // 将整型变量从主机字节顺序(小端)转变成网络字节顺序(大端)
    address.sin_port = htons(port);

    // 给socket命名
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    // 监听socket 创建一个监听队列以存放待处理的客户连接
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 指定事件
    epoll_event events[MAX_EVENT_NUMBER];
    // 文件描述符指示内核事件表(提示大小)
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    // 将文件描述符listenfd上的某个事件注册到epollfd指示的内核事件表 指定是否对fd启用ET模式
    addfd(epollfd, listenfd, false);
    // 所有socket上的事件都被注册到同一个epoll内核事件表中，所以设置为静态的
    http_conn::m_epollfd = epollfd;

    // 创建信号处理函数与主线程通信的管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    // 写端非阻塞
    setnonblocking(pipefd[1]);
    // 注册管道读端的可读事件
    addfd(epollfd, pipefd[0], false);

    //设置信号处理函数
    // 闹钟超时引起
    addsig(SIGALRM, sig_handler, false);
    // 终止进程，kill命令默认信号
    addsig(SIGTERM, sig_handler, false);

    // 预先为每个客户连接分配的定时器？？？
    client_data *users_timer = new client_data[MAX_FD];

    bool stop_server = false;
    bool timeout = false;
    // TIMESLOT秒后将信号SIGALARM发到当前进程
    alarm(TIMESLOT);

    while (!stop_server)
    {
        // epoll_wait返回就绪的文件描述符的个数
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            printf("fd:%d event:",sockfd);
            if (sockfd == listenfd) // 新的连接请求
            {
                printf("incoming socket\n");
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef LT
                // 接受连接，获取被接受的远程socket地址
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                // 用socket值来做http_conn对象的索引 并初始化http_conn
                users[connfd].init(connfd, client_address);
                // 初始化client_data
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                // 该连接的定时器 升序定时器链表的节点
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                // 将timer插入到升序定时器链表
                timer_lst.add_timer(timer);
#endif

#ifdef ET
                // 边沿触发 必须立即全部读完 后续epoll_wait不再通知
                while (1)
                {
                    // 接受连接，获取被接受的远程socket地址
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        printf("errno is: %d\n", errno);
                        break;//!!!!!!!!!!!!!!!!!!
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        break;//!!!!!!!!!!!!!!!!!!!
                    }
                    // 用socket值来做http_conn对象的索引 并初始化http_conn
                    users[connfd].init(connfd, client_address);
                    // 初始化client_data
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    // 该连接的定时器 升序定时器链表的节点
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    // 将timer插入到升序定时器链表
                    timer_lst.add_timer(timer);
                }
                continue;//!!!!!!!!!!!!!!!!!!!!!!!!!!!!
#endif
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) // 连接socket的事件:挂起、被对方关闭、错误
            {
                printf("被关闭/挂起/错误\n");
                // users[sockfd].close_conn();
                // 获取连接对应的timer
                util_timer *timer = users_timer[sockfd].timer;
                // 关闭连接
                timer->cb_func(&users_timer[sockfd]);
                // 移除对应定时器
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }                
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))// 管道读就绪
            {
                printf("incoming signals\n");
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)// 读管道出错
                {
                    continue;
                }
                else if (ret == 0)// 管道被对方关闭？
                {
                    continue;
                }
                else// 处理信号
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:// 定时器超时
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:// 终止进程
                            {
                                stop_server = true;
                            }
                        }
                    }
                }                
            }
            else if (events[i].events & EPOLLIN) // 读就绪
            {
                printf("socket读就绪\n");
                // 获取连接对应timer
                util_timer *timer = users_timer[sockfd].timer;
                // 根据读的结果决定是将任务添加到线程池还是关闭连接
                if (users[sockfd].read()) // 从socket对应内核读缓冲区中非阻塞读
                {
                    pool->append(users + sockfd); // 往线程池的请求队列中添加任务
                    // 读成功 定时器重置 并调整其在链表上的位置
                    if (timer)
                    {
                        printf("定时器重置\n");
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                else// 读错误 需要关闭连接
                {
                    // 关闭连接
                    timer->cb_func(&users_timer[sockfd]);
                    // 移除对应定时器
                    if (timer)
                    {
                        timer_lst.del_timer(timer);   
                    }         
                }
            }
            else if (events[i].events & EPOLLOUT) // 写就绪
            {
                printf("socket写就绪\n");
                // 获取连接对应timer
                util_timer *timer = users_timer[sockfd].timer;
                // 根据写的结果决定是否关闭连接
                if (users[sockfd].write()) // 从socket对应内核写缓冲区中非阻塞写
                {
                    // 写成功 定时器重置 并调整其在链表上的位置
                    if (timer)
                    {
                        printf("定时器重置\n");
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                else// 写错误/connection:close 需要关闭连接
                {
                    // 关闭连接
                    timer->cb_func(&users_timer[sockfd]);
                    // 移除对应定时器
                    if (timer)
                    {
                        timer_lst.del_timer(timer);   
                    }         
                }
            }
            else
            {
                printf("error:unknown event\n");
            }
        }
        // 最后处理定时事件，应为I/O事件有着更高的优先级
        // 同时也导致定时任务不能精确的按照预期时间执行
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
        
    }

    close(epollfd);  // 关闭内核事件表的文件描述符
    close(listenfd); // 关闭监听socket的文件描述符
    close(pipefd[0]); // 关闭管道
    close(pipefd[1]);
    delete[] users;  // 释放http_conn对象数组
    delete[] users_timer;  // 释放client_data对象数组
    delete pool;     // 释放线程池
    return 0;
}
