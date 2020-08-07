#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>

#define BUFFER_SIZE 64
class util_timer;

// 用户数据结构：客户端socket地址、socket文件描述符、读缓存、定时器
struct client_data
{
    sockaddr_in address;
    int sockfd;
    char buf[BUFFER_SIZE];
    util_timer *timer;
};

// 定时器 升序定时器链表的节点
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                  // 任务的超时时间
    void (*cb_func)(client_data *); // 任务回调函数
    client_data *user_data;         // 用户数据结构：客户端socket地址、socket文件描述符、读缓存、定时器
    util_timer *prev;               // 指向前一个定时器
    util_timer *next;               // 指向后一个定时器
};

class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL), size(0) {}

    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    void add_timer(util_timer *timer)
    {
        if (!timer)// timer空
        {
            return;
        }
        size++;
        if (!head)// 链表为空
        {
            head = tail = timer;
            return;
        }
        if (timer->expire < head->expire)// 马上超时,插到头部
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }

    void adjust_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        // 加时以后仍然小于后一个定时器
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        // 真的需要调整
        // 要移动的是头结点
        if (timer == head)
        {
            // 改变头结点,跳过自己
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            // 从下一个节点往后找
            add_timer(timer, head);
        }
        else
        {
            // 连接前后连个节点,跳过自己
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            // 从下一个节点往后找
            add_timer(timer, timer->next);
        }
    }

    void del_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        size--;
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    void tick()
    {
        if (!head)
        {
            return;
        }
        printf("timer tick\n");
        time_t cur = time(NULL);
        util_timer *tmp = head;
        while (tmp)
        {
            if (cur < tmp->expire)// 还没超时
            {
                break;
            }
            tmp->cb_func(tmp->user_data);
            size--;
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

    int get_list_size()
    {
        return size;
    }

private:
    void add_timer(util_timer *timer, util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *cur = prev->next;
        while (cur)
        {
            // 找到要插入的位置
            if (timer->expire < cur->expire)
            {
                prev->next = timer;
                timer->next = cur;
                cur->prev = timer;
                timer->prev = prev;
                break;
            }
            // 都往后走
            prev = cur;
            cur = cur->next;
        }
        // 插到尾部
        if (!cur)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer *head;
    util_timer *tail;
    int size;
};

#endif
