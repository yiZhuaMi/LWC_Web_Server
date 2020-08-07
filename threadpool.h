#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

template <typename T>
class threadpool
{
public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);

private:
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        // 线程池中的线程数
    int m_max_requests;         // 请求队列中允许的最大的等待请求数
    pthread_t *m_threads;       // 线程标识符的数组，其大小为m_thread_number
   
    std::list<T *> m_workqueue; // 请求队列
    locker m_queuelocker;       // 保护请求队列的互斥锁
    sem m_queuestat;            // 是否有任务需要处理
    bool m_stop;                // 是否结束线程
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL)
{
    if ((thread_number <= 0) || (max_requests <= 0))
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number]; // 线程标识符的数组
    if (!m_threads)
    {
        throw std::exception();
    }

    // 创建thread_number个线程，并都设置为脱离线程
    for (int i = 0; i < thread_number; ++i)
    {
        printf("create the %dth thread\n", i);
        // (新线程的标识符,新线程的属性,新线程将运行的函数,新线程将运行的函数的参数)
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request); // 往线程池的请求队列中添加任务

    static int max_size = 0;
    max_size = m_workqueue.size() > max_size ? m_workqueue.size() : max_size;
    printf("请求队列size:%d max_size:%d\n", m_workqueue.size(), max_size);

    m_queuelocker.unlock();
    m_queuestat.post(); // 释放信号量 让信号量的值加1
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        m_queuestat.wait();   // 阻塞等待任务 允许多个线程进入临界区 再争抢锁
        m_queuelocker.lock(); // 线程竞争 给请求队列加锁
        // 应对惊群效应
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front(); // 获取队头的请求：http_conn对象
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }
        request->process(); // request = users + sockfd
    }
}

#endif
