#include "task_queue.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define MAX_TASKS 16
#define MAX_WORKERS 1

struct _Task
{
    TaskFunc function;
    void* arg;
    pthread_mutex_t mutex;
    pthread_cond_t cond_done;
    int done;
    atomic_int ref_count;
};

struct _TaskQueue
{
    Task* tasks[MAX_TASKS];
    int head;
    int tail;
    int stop;
    int producers;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    pthread_cond_t cond_producers;
    TaskQueueWorkerFactory worker_factory;
    int worker_count;
    TaskQueueWorker* workers[MAX_WORKERS];
};

static Task* task_new(TaskFunc function, void* arg)
{
    Task* task = calloc(1, sizeof(Task));
    if (!task) {
        return NULL;
    }
    task->function = function;
    task->arg = arg;
    if (pthread_mutex_init(&task->mutex, NULL) != 0) {
        free(task);
        return NULL;
    }
    if (pthread_cond_init(&task->cond_done, NULL) != 0) {
        pthread_mutex_destroy(&task->mutex);
        free(task);
        return NULL;
    }
    task->done = 0;
    atomic_init(&task->ref_count, 1);
    return task;
}

void task_addref(Task* task)
{
    if (!task) {
        return;
    }
    atomic_fetch_add(&task->ref_count, 1);
}

void task_release(Task* task)
{
    if (!task) {
        return;
    }
    if (atomic_fetch_sub(&task->ref_count, 1) == 1) {
        pthread_mutex_destroy(&task->mutex);
        pthread_cond_destroy(&task->cond_done);
        free(task);
    }
}

void task_invoke(Task* task)
{
    if (!task) {
        return;
    }
    pthread_mutex_lock(&task->mutex);
    if (task->function) {
        task->function(task->arg);
    }
    task->done = 1;
    pthread_cond_signal(&task->cond_done);
    pthread_mutex_unlock(&task->mutex);
}

static void task_cancel(Task* task)
{
    if (!task) {
        return;
    }
    pthread_mutex_lock(&task->mutex);
    task->done = 1;
    pthread_cond_signal(&task->cond_done);
    pthread_mutex_unlock(&task->mutex);
}

void task_wait(Task* task)
{
    if (!task) {
        return;
    }
    pthread_mutex_lock(&task->mutex);
    while (!task->done) {
        pthread_cond_wait(&task->cond_done, &task->mutex);
    }
    pthread_mutex_unlock(&task->mutex);
}

TaskQueue* task_queue_new(TaskQueueWorkerFactory worker_factory)
{
    if (!worker_factory) {
        return NULL;
    }

    TaskQueue* queue = calloc(1, sizeof(TaskQueue));
    if (!queue) {
        return NULL;
    }
    queue->head = 0;
    queue->tail = 0;
    queue->stop = 0;
    queue->producers = 0;
    queue->worker_factory = worker_factory;
    queue->worker_count = 0;

    // The worker starts consuming as soon as it is created, so all queue
    // synchronization primitives must be initialized before CreateThread.
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue);
        return NULL;
    }
    if (pthread_cond_init(&queue->cond_not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    if (pthread_cond_init(&queue->cond_not_full, NULL) != 0) {
        pthread_cond_destroy(&queue->cond_not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    if (pthread_cond_init(&queue->cond_producers, NULL) != 0) {
        pthread_cond_destroy(&queue->cond_not_full);
        pthread_cond_destroy(&queue->cond_not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }

    TaskQueueWorker* worker = worker_factory(queue);
    if (!worker) {
        pthread_cond_destroy(&queue->cond_producers);
        pthread_cond_destroy(&queue->cond_not_full);
        pthread_cond_destroy(&queue->cond_not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    queue->workers[queue->worker_count++] = worker;
    return queue;
}

void task_queue_free(TaskQueue* queue)
{
    if (!queue) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->stop = 1;
    pthread_cond_broadcast(&queue->cond_not_empty);
    pthread_cond_broadcast(&queue->cond_not_full);
    pthread_mutex_unlock(&queue->mutex);

    for (int i = 0; i < queue->worker_count; i++) {
        TaskQueueWorker* worker = queue->workers[i];
        if (!worker) {
            continue;
        }
        worker->join(worker);
        worker->release(worker);
        queue->workers[i] = NULL;
    }

    // Destroyed is emitted after MaaCore has joined its callback threads, so
    // no new producer can enter. Wait for one that was already blocked in
    // task_queue_push before tearing down the synchronization primitives.
    pthread_mutex_lock(&queue->mutex);
    while (queue->producers > 0) {
        pthread_cond_wait(&queue->cond_producers, &queue->mutex);
    }
    while (queue->head != queue->tail) {
        Task* task = queue->tasks[queue->head];
        queue->head = (queue->head + 1) % MAX_TASKS;
        task_cancel(task);
        task_release(task);
    }
    pthread_mutex_unlock(&queue->mutex);

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_not_empty);
    pthread_cond_destroy(&queue->cond_not_full);
    pthread_cond_destroy(&queue->cond_producers);
    free(queue);
}

static void producer_leave(TaskQueue* queue)
{
    // called with queue->mutex held
    queue->producers--;
    if (queue->producers == 0) {
        pthread_cond_signal(&queue->cond_producers);
    }
}

Task* task_queue_push(TaskQueue* queue, TaskFunc function, void* arg)
{
    if (!queue) {
        return NULL;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->producers++;
    while (!queue->stop && (queue->tail + 1) % MAX_TASKS == queue->head) {
        pthread_cond_wait(&queue->cond_not_full, &queue->mutex);
    }
    if (queue->stop) {
        producer_leave(queue);
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    Task* task = task_new(function, arg);
    if (!task) {
        producer_leave(queue);
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    // Take the queue's reference before the task becomes visible to the
    // worker. Otherwise the worker can pop, invoke and release the task
    // while the caller still holds the only reference, freeing it under
    // the caller's feet.
    task_addref(task);

    queue->tasks[queue->tail] = task;
    queue->tail = (queue->tail + 1) % MAX_TASKS;

    producer_leave(queue);
    pthread_cond_signal(&queue->cond_not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return task;
}

Task* task_queue_pop(TaskQueue* queue)
{
    if (!queue) {
        return NULL;
    }

    pthread_mutex_lock(&queue->mutex);
    while (!queue->stop && queue->head == queue->tail) {
        pthread_cond_wait(&queue->cond_not_empty, &queue->mutex);
    }
    if (queue->stop) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    Task* task = queue->tasks[queue->head];
    queue->head = (queue->head + 1) % MAX_TASKS;

    pthread_cond_signal(&queue->cond_not_full);
    pthread_mutex_unlock(&queue->mutex);

    return task;
}
