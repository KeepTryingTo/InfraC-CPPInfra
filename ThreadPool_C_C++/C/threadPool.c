#include "threadPool.h"
#include <pthread.h>
#include <unistd.h>



const int max = 100;
const int NUMBER = 2;

//任务结构体
typedef struct Task {
    void (*function)(void * arg);
    void * arg;
}Task;

//线程池结构体
struct ThreadPool {
    // 任务队列
    Task * taskQ;
    int queueCapacity; //容量
    int queueSize; // 当前任务个数
    int queueFront; // 队头 -> 取数据
    int queueRear; // 队尾 -> 放数据

    pthread_t managerID; // 管理者线程ID
    pthread_t * threadIDs; // 工作的线程ID
    int minNum; //最小线程数
    int maxNum; // 最大线程数
    int busyNum; // 忙的线程个数
    int liveNum; // 存活的线程个数
    int exitNum; // 要销毁的线程个数

    pthread_mutex_t mutexPool; // 锁整个的线程池
    pthread_mutex_t mutexBusy; //  锁busy线程
    pthread_cond_t notFull; // 任务队列是不是满了
    pthread_cond_t notEmpty; // 任务队列是不是为空了

    int shutdown; //是不是要销毁线程池，销毁为1，否则为0
};


ThreadPool * threadPoolCreate(int minNum, int maxNum, int queueSize){
    ThreadPool * pool = (ThreadPool*)malloc(sizeof(ThreadPool));
    do {
        if(pool == NULL){
            printf("malloc threadpool is failed!...\n");
            break;
        }
    
        pool->threadIDs = (pthread_t*)malloc(sizeof(pthread_t) * max);
        if(pool -> threadIDs == NULL){
            printf("malloc threadIDs failed ...\n");
            break;
        }
    
        memset(pool -> threadIDs, 0, sizeof(pthread_t) * max);
        pool -> minNum = minNum;
        pool -> maxNum = maxNum;
        pool -> busyNum = 0;
        pool -> liveNum = minNum;
        pool -> exitNum = 0;
    
        if( pthread_mutex_init(&pool -> mutexPool, NULL) != 0 ||
            pthread_mutex_init(&pool -> mutexBusy, NULL) != 0 ||
            pthread_cond_init(&pool -> notEmpty, NULL) != 0 ||
            pthread_cond_init(&pool -> notFull, NULL) != 0){
                printf("mutex or condition init fail ...\n");
                break;
            }
        
        // 任务队列
        pool -> taskQ = (Task*)malloc(sizeof(Task) * queueSize);
        pool -> queueCapacity = queueSize;
        pool -> queueSize = 0;
        pool -> queueFront = 0;
        pool -> queueRear = 0;
    
        pool -> shutdown = 0;
    
        //创建线程
        pthread_create(&pool -> managerID, NULL, manager, pool);
        for(int i = 0; i < minNum; i++){
            pthread_create(&pool -> threadIDs[i], NULL, worker, pool);
        }
        return pool;
    }while(0);

    //释放资源
    if(pool && pool -> threadIDs)free(pool -> threadIDs);
    if(pool && pool -> taskQ)free(pool -> taskQ);
    if(pool)free(pool);
    return NULL;
}

void * manager(void * arg) {
    ThreadPool* pool = (ThreadPool*)arg;
    while(!pool -> shutdown){
        //每隔3秒检测一次
        sleep(3);

        //取出线程池中任务数量以及当前线程数量
        pthread_mutex_lock(&pool -> mutexPool);
        int queueSize = pool -> queueSize;
        int liveNum = pool -> liveNum;
        pthread_mutex_unlock(&pool -> mutexPool);

        //取出忙的线程数量
        pthread_mutex_lock(&pool -> mutexBusy);
        int busyNum = pool -> busyNum;
        pthread_mutex_unlock(&pool -> mutexBusy);

        // 添加线程
        if(queueSize > liveNum && liveNum < pool -> maxNum){
            pthread_mutex_lock(&pool -> mutexPool);
            int counter = 0;
            for(int i = 0; i < pool -> maxNum && counter < NUMBER
                    && pool -> liveNum < pool -> maxNum; i++){
                if(pool -> threadIDs[i] == 0){
                    pthread_create(&pool -> threadIDs[i], NULL, worker, pool);
                    counter += 1;
                    pool -> liveNum += 1;
                } 
            }
            pthread_mutex_unlock(&pool -> mutexPool);
        }

        //销毁线程
        if(busyNum * 2 < liveNum && liveNum > pool -> minNum){
            pthread_mutex_lock(&pool -> mutexPool);
            pool -> exitNum = NUMBER;
            pthread_mutex_unlock(&pool -> mutexPool);

            for(int i = 0; i < NUMBER; i++){
                pthread_cond_signal(&pool -> notEmpty);
            }
        }
    }
}


void * worker(void * arg) {
    ThreadPool * pool = (ThreadPool*)arg;

    while(1) {
        pthread_mutex_lock(&pool -> mutexPool);
        while(pool -> queueSize == 0 && !pool -> shutdown){
            pthread_cond_wait(&pool -> notEmpty, &pool -> mutexPool);

            // 判断是不是要销毁线程
            if(pool -> exitNum > 0){
                pool -> exitNum -= 1;
                if(pool -> liveNum > pool -> minNum){
                    pool -> liveNum -= 1;
                    pthread_mutex_unlock(&pool -> mutexPool);
                    threadDestroy(pool);
                }
            }
        }

        //判断线程池是否被关闭
        if(pool -> shutdown){
            pthread_mutex_unlock(&pool -> mutexPool);
            threadDestroy(pool);
        }

        //从任务队列中取出一个任务
        Task task;
        task.function = pool -> taskQ[pool -> queueFront].function;
        task.arg = pool -> taskQ[pool -> queueFront].arg;
        //循环队列
        pool -> queueFront = (pool -> queueFront + 1) % pool -> queueCapacity;
        pool -> queueSize -= 1;
        //唤醒生产者
        pthread_cond_signal(&pool -> notFull);

        pthread_mutex_unlock(&pool -> mutexPool);

        printf("thread %ld start working...\n", pthread_self());
        pthread_mutex_lock(&pool -> mutexBusy);
        pool -> busyNum += 1;
        pthread_mutex_unlock(&pool -> mutexBusy);
        task.function(task.arg);
        free(task.arg);
        task.arg = NULL;

        printf("thread %ld end working...\n", pthread_self());
        pthread_mutex_lock(&pool -> mutexBusy);
        pool -> busyNum -= 1;
        pthread_mutex_unlock(&pool -> mutexBusy);


    }
}

void threadPoolAdd(ThreadPool * pool, void(*func)(void *), void *arg) {
    pthread_mutex_lock(&pool -> mutexPool);
    while(pool -> queueSize == pool -> queueCapacity && !pool -> shutdown){
        //阻塞生成者，由于当前的任务数已经达到上限了
        pthread_cond_wait(&pool -> notFull, &pool -> mutexPool);
    }
    if(pool -> shutdown){
        pthread_mutex_unlock(&pool -> mutexPool);
    }

    // 添加任务
    pool -> taskQ[pool -> queueRear].function = func;
    pool -> taskQ[pool -> queueRear].arg = arg;
    pool -> queueRear = (pool -> queueRear + 1) % pool -> queueCapacity;
    pool -> queueSize += 1;

    pthread_cond_signal(&pool -> notEmpty);
    pthread_mutex_unlock(&pool -> mutexPool);
}

int threadPoolBusyNum(ThreadPool * pool){
    pthread_mutex_lock(&pool -> mutexBusy);
    int busyNum = pool -> busyNum;
    pthread_mutex_unlock(&pool -> mutexBusy);
    return busyNum;
}
int threadPoolAliveNum(ThreadPool * pool) {
    pthread_mutex_lock(&pool -> mutexPool);
    int aliveNum = pool -> liveNum;
    pthread_mutex_unlock(&pool -> mutexPool);
    return aliveNum;
}

void threadDestroy(ThreadPool * pool) {
    pthread_t pid = pthread_self();
    for(int i = 0; i < pool -> maxNum; i++){
        if(pool -> threadIDs[i] == pid) {
            pool -> threadIDs[i] = 0;
            printf("thread destroy called, %ld exiting...\n", pid);
            break;
        }
    }
    pthread_exit(NULL);
}

int threadPoolDestroy(ThreadPool * pool) {
    if(pool == NULL){
        return -1;
    }

    //关闭线程池
    pool -> shutdown = 1;
    //阻塞回收管理者线程
    pthread_join(pool -> managerID, NULL);
    //唤醒阻塞的消费者线程
    for(int i = 0; i < pool -> liveNum; i++){
        pthread_cond_signal(&pool -> notEmpty);
    }
    //释放堆内存
    if(pool -> taskQ)free(pool -> taskQ);
    if(pool -> threadIDs)free(pool -> threadIDs);
    pthread_mutex_destroy(&pool -> mutexBusy);
    pthread_mutex_destroy(&pool -> mutexPool);
    pthread_cond_destroy(&pool -> notEmpty);
    pthread_cond_destroy(&pool -> notFull);

    free(pool);
    pool = NULL;
    return 0;
}