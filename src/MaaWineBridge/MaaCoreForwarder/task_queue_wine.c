#include "task_queue_wine.h"
#include <stdatomic.h>
#include <stdlib.h>

// clang-format off
#include <windef.h>
#include <winbase.h>

// clang-format on

typedef struct _TaskQueueWorkerWine
{
    TaskQueueWorker base;
    atomic_int ref_count;
    HANDLE thread;
} TaskQueueWorkerWine;

static void task_queue_worker_wine_release(TaskQueueWorker* worker)
{
    TaskQueueWorkerWine* worker_wine = (TaskQueueWorkerWine*)worker;
    if (atomic_fetch_sub(&worker_wine->ref_count, 1) == 1) {
        if (worker_wine->thread) {
            CloseHandle(worker_wine->thread);
        }
        free(worker_wine);
    }
}

static void task_queue_worker_wine_join(TaskQueueWorker* worker)
{
    TaskQueueWorkerWine* worker_wine = (TaskQueueWorkerWine*)worker;
    if (!worker_wine->thread) {
        return;
    }
    // Wine 线程的 pthread_t 在线程启动后才可知，改为等待 CreateThread 句柄。
    const DWORD wait_result = WaitForSingleObject(worker_wine->thread, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        // 句柄在 release 前始终有效；等待失败说明生命周期有误，继续释放会导致 UAF。
        abort();
    }
}

static DWORD WINAPI worker(void* arg)
{
    TaskQueueWorkerWine* worker = arg;
    TaskQueue* queue = worker->base.queue;
    while (1) {
        Task* task = task_queue_pop(queue);
        if (task == NULL) {
            break;
        }
        task_invoke(task);
        task_release(task);
    }
    return 0;
}

TaskQueueWorker* task_queue_worker_wine_new(TaskQueue* queue)
{
    TaskQueueWorkerWine* worker_wine = calloc(1, sizeof(TaskQueueWorkerWine));
    if (!worker_wine) {
        return NULL;
    }
    worker_wine->base.queue = queue;
    worker_wine->base.release = task_queue_worker_wine_release;
    worker_wine->base.join = task_queue_worker_wine_join;
    atomic_init(&worker_wine->ref_count, 1);

    worker_wine->thread = CreateThread(NULL, 0, worker, worker_wine, 0, NULL);
    if (!worker_wine->thread) {
        free(worker_wine);
        return NULL;
    }
    return (TaskQueueWorker*)worker_wine;
}
