#pragma once

typedef struct _Task Task;
typedef struct _TaskQueue TaskQueue;
typedef struct _TaskQueueWorker TaskQueueWorker;

struct _TaskQueueWorker
{
    TaskQueue* queue;
    void (*release)(TaskQueueWorker*);
    void (*join)(TaskQueueWorker*);
};

typedef void (*TaskFunc)(void*);
typedef TaskQueueWorker* (*TaskQueueWorkerFactory)(TaskQueue*);

// 生产者契约：调用方须自行串行化；task_queue_free 只能在最后一次 push 返回后调用。
// 目前唯一生产者是 MaaCore 回调：普通消息都来自消息线程，Destroyed 在消息线程
// join 后才由销毁线程投递，因此 free 发生时不会有其它 push 在途。free 内部的
// producers 只能等待已进入 mutex 的调用方，管不到已取得指针但尚未加锁的调用方。

// Ok：已入队；Closed：队列正在关闭；Failed：任务或同步对象分配失败。
typedef enum
{
    TaskQueuePushOk,
    TaskQueuePushClosed,
    TaskQueuePushFailed,
} TaskQueuePushStatus;

TaskQueue* task_queue_new(TaskQueueWorkerFactory worker_factory);
void task_queue_free(TaskQueue* queue);
Task* task_queue_push(TaskQueue* queue, TaskFunc function, void* arg, TaskQueuePushStatus* status);
Task* task_queue_pop(TaskQueue* queue);

void task_wait(Task* task);
void task_invoke(Task* task);
void task_addref(Task* task);
void task_release(Task* task);
