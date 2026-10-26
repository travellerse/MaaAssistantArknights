#include "dl_maacore.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// clang-format off
// don't sort includes
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
// clang-format on

#include "task_queue.h"
#include "task_queue_wine.h"

#define AsstMsg_Destroyed (5)

static char* translate_utf8_path(const char* utf8_win_path)
{
    if (!utf8_win_path) {
        return NULL;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8_win_path, -1, NULL, 0);
    if (len <= 0) {
        return NULL;
    }
    wchar_t* wpath = malloc((size_t)len * sizeof(wchar_t));
    if (!wpath) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8_win_path, -1, wpath, len);
    char* unix_path = wine_get_unix_file_name(wpath);
    free(wpath);
    return unix_path;
}

static int win32_free(void* ptr)
{
    return HeapFree(GetProcessHeap(), 0, ptr);
}

// GUI 看不到 stderr，但 asst.log 里能看到；libMaaCore 没加载时退回 stderr。
// 等级用 MaaCore 的短标签（Logger::level::warn 就是 "WRN"）。
// 调用点都在实例创建之后，UserDir 已设；AsstLog 在 UserDir 为空时会丢弃消息。
static void log_warn(const char* message)
{
    if (dl_AsstLog) {
        dl_AsstLog("WRN", message);
    }
    else {
        fprintf(stderr, "MaaWineBridge: %s\n", message);
    }
}

typedef void(
    __stdcall* AsstApiCallbackMsAbi)(AsstMsgId msg, const char* details_json, void* custom_arg);

typedef struct
{
    AsstApiCallbackMsAbi x64abi_callback;
    void* custom_arg;
    TaskQueue* queue;
    AsstHandle handle;
} CallbackState;

typedef struct
{
    CallbackState* state;
    AsstMsgId msg;
    const char* details_json;
} CallbackInvocation;

static CallbackState* cb_state_new()
{
    CallbackState* fwd_state = calloc(1, sizeof(CallbackState));
    if (!fwd_state) {
        return NULL;
    }
    TaskQueue* queue = task_queue_new(task_queue_worker_wine_new);
    if (!queue) {
        free(fwd_state);
        return NULL;
    }
    fwd_state->queue = queue;
    return fwd_state;
}

static void cb_state_free(CallbackState* fwd_state)
{
    if (!fwd_state) {
        return;
    }
    task_queue_free(fwd_state->queue);
    free(fwd_state);
}

// 记录当前 Wine 线程正在执行哪个实例的回调：只有同一实例的同步重入不能入队，
// 跨实例重入由对方 worker 执行，释放路径保持完整。
static _Thread_local CallbackState* tls_active_state = NULL;

static void invoke_helper(void* arg)
{
    // called in win32 thread
    CallbackInvocation* invocation = arg;
    CallbackState* state = invocation->state;
    if (state->x64abi_callback) {
        CallbackState* previous = tls_active_state;
        tls_active_state = state;
        state->x64abi_callback(invocation->msg, invocation->details_json, state->custom_arg);
        tls_active_state = previous;
    }
}

// 回调必须快速返回：消息线程在这里等 worker 执行完，卡住会让整个实例静默停摆。
static void UnixCallbackShim(AsstMsgId msg, const char* details_json, void* custom_arg)
{
    CallbackState* state = (CallbackState*)custom_arg;
    CallbackInvocation invocation = {
        .state = state,
        .msg = msg,
        .details_json = details_json,
    };
    if (tls_active_state == state) {
        // 本实例 worker 上的同步重入：入队会等待自身，只能直接调用。
        if (msg == AsstMsg_Destroyed) {
            // ~Assistant 先 join 消息线程才发出 Destroyed，同实例销毁应到不了这里；
            // 真到这里时释放会 join 当前 worker，只能记录泄漏。
            log_warn("Destroyed re-entered on its own worker");
        }
        invoke_helper(&invocation);
        return;
    }
    TaskQueuePushStatus push_status = TaskQueuePushFailed;
    Task* task = task_queue_push(state->queue, invoke_helper, &invocation, &push_status);
    if (task) {
        task_wait(task);
        task_release(task);
    }
    else if (push_status == TaskQueuePushFailed) {
        log_warn("callback dropped: task allocation failed");
    }
    // 入队失败时丢弃回调：Closed 属于正常关闭，Failed 表示资源耗尽；
    // Destroyed 仍会释放 state。
    if (msg == AsstMsg_Destroyed) {
        cb_state_free(state);
    }
}

AsstBool __stdcall WineShimAsstSetUserDir(const char* path)
{
    char* unix_path = translate_utf8_path(path);
    if (!unix_path) {
        return 0;
    }
    AsstBool result = dl_AsstSetUserDir(unix_path);
    win32_free(unix_path);
    return result;
}

AsstBool __stdcall WineShimAsstLoadResource(const char* path)
{
    char* unix_path = translate_utf8_path(path);
    if (!unix_path) {
        return 0;
    }
    AsstBool result = dl_AsstLoadResource(unix_path);
    win32_free(unix_path);
    return result;
}

AsstBool __stdcall WineShimAsstSetStaticOption(AsstStaticOptionKey key, const char* value)
{
    return dl_AsstSetStaticOption(key, value);
}

AsstHandle __stdcall WineShimAsstCreate()
{
    return dl_AsstCreate();
}

AsstHandle __stdcall WineShimAsstCreateEx(AsstApiCallbackMsAbi callback, void* custom_arg)
{
    CallbackState* fwd_state = cb_state_new();
    if (!fwd_state) {
        return NULL;
    }
    fwd_state->x64abi_callback = callback;
    fwd_state->custom_arg = custom_arg;
    // callback needs to be called in a wine thread
    AsstHandle result = dl_AsstCreateEx(UnixCallbackShim, fwd_state);
    if (!result) {
        cb_state_free(fwd_state);
    }
    else {
        fwd_state->handle = result;
    }
    return result;
}

void __stdcall WineShimAsstDestroy(AsstHandle handle)
{
    // 不能在回调内同步销毁同实例：析构会 join 消息线程，而消息线程正等本次回调返回，
    // 直接调用将永久互相等待（Windows 侧表现为析构里 join 自身，同属非法用法）。
    // 这里只停机不销毁：句柄仍然有效，持有它的调用方可以在回调返回后重新发起；
    // 已经丢弃句柄的调用方会泄漏这个实例。
    if (handle && tls_active_state && tls_active_state->handle == handle) {
        log_warn(
            "AsstDestroy from its own callback ignored: instance NOT destroyed, "
            "call it again after the callback returns");
        dl_AsstStop(handle);
        return;
    }
    dl_AsstDestroy(handle);
}

AsstBool __stdcall WineShimAsstSetInstanceOption(
    AsstHandle handle,
    AsstInstanceOptionKey key,
    const char* value)
{
    return dl_AsstSetInstanceOption(handle, key, value);
}

AsstBool __stdcall WineShimAsstConnect(
    AsstHandle handle,
    const char* adb_path,
    const char* address,
    const char* config)
{
    if (!address) {
        return 0;
    }
    char* unix_adb_path = translate_utf8_path(adb_path);
    if (!unix_adb_path) {
        return 0;
    }
    AsstBool result = dl_AsstConnect(handle, unix_adb_path, address, config);
    win32_free(unix_adb_path);
    return result;
}

AsstTaskId __stdcall WineShimAsstAppendTask(AsstHandle handle, const char* type, const char* params)
{
    return dl_AsstAppendTask(handle, type, params);
}

AsstBool __stdcall WineShimAsstSetTaskParams(AsstHandle handle, AsstTaskId id, const char* params)
{
    return dl_AsstSetTaskParams(handle, id, params);
}

AsstBool __stdcall WineShimAsstStart(AsstHandle handle)
{
    return dl_AsstStart(handle);
}

AsstBool __stdcall WineShimAsstStop(AsstHandle handle)
{
    return dl_AsstStop(handle);
}

AsstBool __stdcall WineShimAsstRunning(AsstHandle handle)
{
    return dl_AsstRunning(handle);
}

AsstBool __stdcall WineShimAsstConnected(AsstHandle handle)
{
    return dl_AsstConnected(handle);
}

AsstBool __stdcall WineShimAsstBackToHome(AsstHandle handle)
{
    return dl_AsstBackToHome(handle);
}

// Win32 窗口绑定只有 Windows 侧实现，Linux 的 libMaaCore 没有对应能力。
// 返回失败而不是缺符号，避免 P/Invoke 抛 EntryPointNotFoundException。
AsstBool __stdcall WineShimAsstAttachWindow(
    AsstHandle handle,
    void* hwnd,
    uint64_t screencap_method,
    uint64_t mouse_method,
    uint64_t keyboard_method)
{
    return 0;
}

AsstAsyncCallId __stdcall WineShimAsstAsyncAttachWindow(
    AsstHandle handle,
    void* hwnd,
    uint64_t screencap_method,
    uint64_t mouse_method,
    uint64_t keyboard_method,
    AsstBool block)
{
    return 0;
}

AsstAsyncCallId __stdcall WineShimAsstAsyncConnect(
    AsstHandle handle,
    const char* adb_path,
    const char* address,
    const char* config,
    AsstBool block)
{
    if (!adb_path || !address) {
        return 0;
    }
    char* unix_adb_path;
    if (strchr(adb_path, '/') != NULL || strchr(adb_path, '\\') != NULL) {
        unix_adb_path = translate_utf8_path(adb_path);
        if (!unix_adb_path) {
            return 0;
        }
    }
    else {
        unix_adb_path = (char*)adb_path;
    }
    AsstAsyncCallId result = dl_AsstAsyncConnect(handle, unix_adb_path, address, config, block);
    if (unix_adb_path != adb_path) {
        win32_free(unix_adb_path);
    }
    return result;
}

void __stdcall WineShimAsstSetConnectionExtras(const char* name, const char* extras) {
    dl_AsstSetConnectionExtras(name, extras);
}

AsstAsyncCallId __stdcall WineShimAsstAsyncClick(
    AsstHandle handle,
    int32_t x,
    int32_t y,
    AsstBool block)
{
    return dl_AsstAsyncClick(handle, x, y, block);
}

AsstAsyncCallId __stdcall WineShimAsstAsyncScreencap(AsstHandle handle, AsstBool block)
{
    return dl_AsstAsyncScreencap(handle, block);
}

AsstSize __stdcall WineShimAsstGetImage(AsstHandle handle, void* buff, AsstSize buff_size)
{
    return dl_AsstGetImage(handle, buff, buff_size);
}

AsstSize __stdcall WineShimAsstGetImageBgr(AsstHandle handle, void* buff, AsstSize buff_size)
{
    return dl_AsstGetImageBgr(handle, buff, buff_size);
}

AsstSize __stdcall WineShimAsstGetUUID(AsstHandle handle, char* buff, AsstSize buff_size)
{
    return dl_AsstGetUUID(handle, buff, buff_size);
}

AsstSize __stdcall WineShimAsstGetTasksList(AsstHandle handle, AsstTaskId* buff, AsstSize buff_size)
{
    return dl_AsstGetTasksList(handle, buff, buff_size);
}

AsstSize __stdcall WineShimAsstGetNullSize()
{
    return dl_AsstGetNullSize();
}

const char* __stdcall WineShimAsstGetVersion()
{
    return dl_AsstGetVersion();
}

void __stdcall WineShimAsstLog(const char* level, const char* message)
{
    dl_AsstLog(level, message);
}
