#include "platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_metal.h>
#if HAVE_WINDOWS
#define SDL_MAIN_HANDLED
#define SDL_MAIN_NOIMPL
#include <SDL3/SDL_main.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif
#if HAVE_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

//
// Types
//

#if HAVE_WINDOWS
struct OS_Socket
{
    SOCKET fd;
    u16    port;
};

struct OS_LockFile
{
    HANDLE handle;
    char   path[OS_MAX_PATH];
};
#else
struct OS_Socket
{
    i32 fd;
    u16 port;
};

struct OS_LockFile
{
    i32  fd;
    char path[OS_MAX_PATH];
};
#endif

// Windows delivers a window's messages only to the thread that created it, and pumping them
// takes every message of the calling thread, the app's included. SDL pumps on its main thread
// only, which on Linux is the app's to block or never to call us on. So on both SDL video lives
// on a thread of its own that owns the windows and pumps them; os_* window calls run there
// (os_run_on_window_thread) and events reach os_poll_events through this queue. macOS has Cocoa
// on the app's main thread instead.
#define HAVE_WINDOW_THREAD    (HAVE_WINDOWS || HAVE_LINUX)
#define OS_WINDOW_EVENT_QUEUE 512
#define OS_EVENT_TEXT_LENGTH  64

namespace
{

struct OS_QueuedEvent
{
    SDL_Event event;
    char      text[OS_EVENT_TEXT_LENGTH]; // the text of text events, which SDL frees on its next pump
};

struct OS_State
{
    b8              initialized;
    b8              headless;
    OS_LogLevel     log_level;
    OS_AssertMode   assert_mode;
    u32             assert_count;
    OS_RawEventHook raw_hook;
    void*           raw_hook_user;
    f32             pushed_x;           // the last synthetic motion, for the next one's relative motion
    f32             pushed_y;
    SDL_WindowID    pushed_window;      // got synthetic mouse events, which then drive its mouse alone
    b8              relative_saved;     // the cursor before relative mode, put back after it
    f32             relative_x;
    f32             relative_y;
    SDL_WindowID    hook_motion_window; // where the hook's last mouse motion was, to give clicks their own
    f32             hook_motion_x;
    f32             hook_motion_y;

    SDL_Mutex*      log_mutex;
    OS_LogCallback* log_callback;
    void*           log_callback_user;
    OS_File*        log_file;      // PUPPETXR_LOG_FILE, opened on first use
    b8              log_file_tried;
    OS_LogLine      log_lines[OS_LOG_HISTORY];
    u32             log_head;
    u32             log_count;
    u64             log_sequence;

    b8              capture_active;
    i32             capture_read;     // pipe read end
    i32             saved_stdout;     // originals, written through; -1 when the process had none
    i32             saved_stderr;
    char            capture_partial[OS_LOG_LINE_LENGTH];
    u32             capture_partial_length;

    SDL_Thread*     window_thread;    // HAVE_WINDOW_THREAD only, see OS_QueuedEvent
    SDL_Semaphore*  window_ready;
    SDL_Mutex*      call_mutex;       // one os_run_on_window_thread call at a time
    SDL_Semaphore*  call_done;
    SDL_Mutex*      call_slot;        // the waiting call; whoever takes it runs it
    OS_WindowThreadFunction* call_function;
    void*           call_user;
    SDL_Semaphore*  window_wake;
    SDL_Mutex*      queue_mutex;
    OS_QueuedEvent* queue;            // OS_WINDOW_EVENT_QUEUE entries, a ring
    u32             wake_event;       // pushed to end the window thread's wait on Linux, never queued
    i32             call_tickets;
    i32             call_ticket;      // of the waiting call, 0 when none
    SDL_AtomicInt   window_waiting;   // the window thread waits in os_mutex_lock
    SDL_AtomicInt   window_quit;
    u32             queue_head;
    u32             queue_count;
    b8              window_thread_ok;
};

struct OS_ListContext
{
    void  (*callback)(const char* name, void* user);
    void* user;
};

} // namespace

//
// Statics
//

static const char* const os_key_names[(u32)OS_Key::Count] = {
    "none",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "space", "escape", "enter", "tab", "backspace",
    "arrow_left", "arrow_right", "arrow_up", "arrow_down",
    "left_shift", "left_ctrl", "left_alt", "right_shift", "right_ctrl", "right_alt",
    "mouse_left", "mouse_right", "mouse_middle",
    "grave", "left_bracket", "right_bracket",
};

#if HAVE_MACOS
// Calls of other threads for the main thread (os_run_on_window_thread, os_post_to_window_thread).
// Created once and kept for the process: a dispatch wakeup may run after os_shutdown
#define OS_MAX_MAIN_CALLS 64

namespace
{

enum class MainCallState : u8
{
    Free,
    Queued,
    Running,
    Done,
};

struct MainCall
{
    OS_WindowThreadFunction* function;
    void*                    user;
    MainCallState            state;
    b8                       posted;
    u64                      order;
};

struct OS_MainQueue
{
    SDL_Mutex*     mutex;
    SDL_Condition* done;
    SDL_Semaphore* wake;          // the main thread waiting for a mutex sleeps on it
    SDL_AtomicInt  main_waiting;
    SDL_AtomicInt  dispatched;    // a wakeup is on the main dispatch queue
    MainCall       calls[OS_MAX_MAIN_CALLS];
    u64            next_order;
    b8             stalled;       // a call was withdrawn; others fail until the main thread serves
    b8             stall_logged;
    b8             full_logged;
};

} // namespace

static OS_MainQueue os_main_queue = {};
#endif

static OS_State os_state = {
    .log_level   = OS_LogLevel::Info,
    .assert_mode = PUPPETXR_DIST ? OS_AssertMode::Log : OS_AssertMode::Break,
};

static usize zm_min_usize (usize a, usize b)
{
    return a < b ? a : b;
}

#if !HAVE_WINDOWS
// strerror is not thread safe; glibc's strerror_r returns the text, POSIX's fills the buffer
namespace
{

struct ErrnoText
{
    char text[128] = {};
};

} // namespace

[[maybe_unused]] static const char* errno_text_result (const char* result, const ErrnoText&)
{
    return result;
}

[[maybe_unused]] static const char* errno_text_result (int result, const ErrnoText& buffer)
{
    return result == 0 ? buffer.text : "unknown error";
}

static ErrnoText errno_text (int error)
{
    ErrnoText buffer;
    const char* text = errno_text_result(strerror_r(error, buffer.text, sizeof(buffer.text)), buffer);
    if (text != buffer.text)
    {
        cstr_copy(buffer.text, sizeof(buffer.text), text);
    }
    return buffer;
}
#endif

#if HAVE_WINDOWS
// Paths and environment values are UTF-8 everywhere in puppetxr; Windows takes UTF-16
static b8 to_wide (const char* text, wchar_t* out, usize capacity)
{
    if (!text || capacity == 0)
    {
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, (int)capacity);
    return length > 0;
}

static b8 from_wide (const wchar_t* text, char* out, usize capacity)
{
    if (!text || capacity == 0)
    {
        return false;
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)capacity, nullptr, nullptr);
    return length > 0;
}
#endif

// File descriptors of the C runtime, for capturing stdout and stderr; the UCRT is shared with
// the app on Windows (CMakeLists.txt), so its descriptors 1 and 2 are the app's
#if HAVE_WINDOWS
static i32 fd_dup (i32 fd)
{
    return _dup(fd);
}

static void fd_dup2 (i32 from, i32 to)
{
    (void)_dup2(from, to);
}

static void fd_close (i32 fd)
{
    (void)_close(fd);
}

static void fd_write (i32 fd, const void* data, usize size)
{
    if (fd >= 0)
    {
        (void)_write(fd, data, (unsigned)size);
    }
}

// Pipes cannot be made non-blocking for _read, so only what is buffered is read
static i64 fd_read_available (i32 fd, void* data, usize capacity)
{
    DWORD available = 0;
    if (!PeekNamedPipe((HANDLE)_get_osfhandle(fd), nullptr, 0, nullptr, &available, nullptr) || available == 0)
    {
        return 0;
    }
    return _read(fd, data, (unsigned)(available < capacity ? available : capacity));
}
#else
static i32 fd_dup (i32 fd)
{
    return dup(fd);
}

static void fd_dup2 (i32 from, i32 to)
{
    (void)dup2(from, to);
}

static void fd_close (i32 fd)
{
    (void)close(fd);
}

static void fd_write (i32 fd, const void* data, usize size)
{
    if (fd >= 0)
    {
        const i64 written = write(fd, data, size);
        UNUSED(written);
    }
}

static i64 fd_read_available (i32 fd, void* data, usize capacity)
{
    return read(fd, data, capacity);
}
#endif

static void log_append (OS_LogLevel level, OS_LogSource source, const char* text)
{
    if (!os_state.log_mutex)
    {
        os_state.log_mutex = SDL_CreateMutex();
    }
    SDL_LockMutex(os_state.log_mutex);
    OS_LogLine& line = os_state.log_lines[(os_state.log_head + os_state.log_count) % OS_LOG_HISTORY];
    if (os_state.log_count == OS_LOG_HISTORY)
    {
        os_state.log_head = (os_state.log_head + 1) % OS_LOG_HISTORY;
    }
    else
    {
        os_state.log_count++;
    }
    line.level   = level;
    line.source  = source;
    line.time_ns = SDL_GetTicksNS();
    cstr_copy(line.text, sizeof(line.text), text);
    os_state.log_sequence++;

    // Hosts like test frameworks and engines often swallow stdout and stderr
    if (!os_state.log_file_tried)
    {
        os_state.log_file_tried = true;
        const char* path        = SDL_getenv_unsafe("PUPPETXR_LOG_FILE");
        os_state.log_file       = path && path[0] ? os_file_append(path) : nullptr;
    }
    if (os_state.log_file)
    {
        static const char* const level_names[] = { "debug", "info", "warning", "error" };
        const char*              origin        = source == OS_LogSource::App ? "app" : level_names[(u32)level];
        char                     prefix[32];
        const usize              length = cstr_format(prefix, sizeof(prefix), "%10.3f %-7s ", (f64)line.time_ns * 1e-9, origin);
        os_file_write(os_state.log_file, prefix, zm_min_usize(length, sizeof(prefix) - 1));
        os_file_write(os_state.log_file, text, SDL_strlen(text));
        os_file_write(os_state.log_file, "\n", 1);
        os_file_flush(os_state.log_file);
    }
    OS_LogCallback* const callback = source == OS_LogSource::Sim ? os_state.log_callback : nullptr;
    void* const           user     = os_state.log_callback_user;
    SDL_UnlockMutex(os_state.log_mutex);

    // Called unlocked so the callback may take its own locks; a line it logs is only kept
    static thread_local b8 in_callback = false;
    if (callback && !in_callback)
    {
        in_callback = true;
        callback(level, text, user);
        in_callback = false;
    }
}

PRINTF_LIKE(3, 0) static void log_prefixed (OS_LogLevel level, const char* prefix, const char* fmt, va_list args)
{
    char message[2048] = {};
    cstr_format_v(message, sizeof(message), fmt, args);
    os_log(level, "%s%s", prefix, message);
}

static SDL_EnumerationResult SDLCALL list_entry (void* user, const char* dirname, const char* fname) // NOLINT(misc-const-correctness): SDL_EnumerateDirectoryCallback
{
    (void)dirname;
    const OS_ListContext* context = (const OS_ListContext*)user;
    context->callback(fname, context->user);
    return SDL_ENUM_CONTINUE;
}

//
// Lifecycle
//

#if HAVE_WINDOW_THREAD
// Events beyond the queue's capacity are dropped, as os_poll_events drops what does not fit
static void queue_event (OS_QueuedEvent& queued)
{
    if (os_state.wake_event && queued.event.type == os_state.wake_event)
    {
        return;
    }
    if (queued.event.type == SDL_EVENT_TEXT_INPUT || queued.event.type == SDL_EVENT_TEXT_EDITING)
    {
        cstr_copy(queued.text, sizeof(queued.text), queued.event.text.text);
    }
    SDL_LockMutex(os_state.queue_mutex);
    // While the app polls nothing, mouse moves and the repeats of a held key would fill the queue
    // and push out the key and button events after them; past half of it repeats go and a move
    // after a move becomes one, its motion summed. Not before: the simulator ignores a single jump
    // of the cursor, and a merged one hides a real move
    if (queued.event.type == SDL_EVENT_KEY_DOWN && queued.event.key.repeat && os_state.queue_count >= OS_WINDOW_EVENT_QUEUE / 2)
    {
        SDL_UnlockMutex(os_state.queue_mutex);
        return;
    }
    if (queued.event.type == SDL_EVENT_MOUSE_MOTION && os_state.queue_count >= OS_WINDOW_EVENT_QUEUE / 2)
    {
        SDL_Event& last = os_state.queue[(os_state.queue_head + os_state.queue_count - 1) % OS_WINDOW_EVENT_QUEUE].event;
        if (last.type == SDL_EVENT_MOUSE_MOTION && last.motion.windowID == queued.event.motion.windowID && last.motion.which == queued.event.motion.which && last.motion.state == queued.event.motion.state)
        {
            const f32 xrel   = last.motion.xrel + queued.event.motion.xrel;
            const f32 yrel   = last.motion.yrel + queued.event.motion.yrel;
            last             = queued.event;
            last.motion.xrel = xrel;
            last.motion.yrel = yrel;
            SDL_UnlockMutex(os_state.queue_mutex);
            return;
        }
    }
    if (os_state.queue_count < OS_WINDOW_EVENT_QUEUE)
    {
        os_state.queue[(os_state.queue_head + os_state.queue_count) % OS_WINDOW_EVENT_QUEUE] = queued;
        os_state.queue_count++;
    }
    SDL_UnlockMutex(os_state.queue_mutex);
}

#if HAVE_WINDOWS
// The window class is registered under our own name and module, so an app using SDL itself
// keeps its class, and the class never outlives the library's window procedure
#define OS_WINDOW_CLASS_STYLE 0x1020 // CS_BYTEALIGNCLIENT | CS_OWNDC, which WGL needs

// A message to the thread ends its wait, and stays queued if it comes before the wait
#define OS_WINDOW_WAIT_MS 100
#else
// SDL wakes a waiting thread only once it waits: a wakeup sent between its look at the queue and
// its wait is lost, and the call then waits out the wait (os_run_on_window_thread)
#define OS_WINDOW_WAIT_MS 10
#endif

static void wake_window_thread ()
{
#if HAVE_WINDOWS
    PostThreadMessageW((DWORD)SDL_GetThreadID(os_state.window_thread), WM_NULL, 0, 0);
#else
    if (os_state.wake_event)
    {
        SDL_Event wake = {};
        wake.type      = os_state.wake_event;
        SDL_PushEvent(&wake);
    }
#endif
}

static int SDLCALL window_thread_main (void* user)
{
    (void)user;
#if HAVE_WINDOWS
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&os_state, &module); // any address in this module
    const b8 registered = SDL_RegisterApp("puppetxr", OS_WINDOW_CLASS_STYLE, module);
#else
    const b8 registered = true;
#endif
    // The first thread to initialize SDL is its main thread, the only one that pumps
    os_state.window_thread_ok = registered && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    if (!os_state.window_thread_ok)
    {
        LOG_WARNING("No display available (%s), continuing headless", SDL_GetError());
#if HAVE_WINDOWS
        if (registered)
        {
            SDL_UnregisterApp();
        }
#endif
        SDL_SignalSemaphore(os_state.window_ready);
        return 0;
    }
    os_state.wake_event = SDL_RegisterEvents(1);
    SDL_SignalSemaphore(os_state.window_ready);

    while (!SDL_GetAtomicInt(&os_state.window_quit))
    {
        OS_QueuedEvent queued = {};
        if (!SDL_WaitEventTimeout(&queued.event, OS_WINDOW_WAIT_MS))
        {
            continue;
        }
        queue_event(queued);
    }
    // The rest of SDL stays up for the other threads until os_shutdown
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
#if HAVE_WINDOWS
    SDL_UnregisterApp();
#endif
    return 0;
}

static b8 start_window_thread ()
{
    os_state.queue       = (OS_QueuedEvent*)os_memory_alloc(sizeof(OS_QueuedEvent) * OS_WINDOW_EVENT_QUEUE);
    os_state.queue_mutex = SDL_CreateMutex();
    os_state.window_ready = SDL_CreateSemaphore(0);
    os_state.call_mutex   = SDL_CreateMutex();
    os_state.call_done    = SDL_CreateSemaphore(0);
    os_state.window_wake  = SDL_CreateSemaphore(0);
    os_state.call_slot    = SDL_CreateMutex();
    os_state.call_ticket  = 0;
    SDL_SetAtomicInt(&os_state.window_quit, 0);
    os_state.queue_head  = 0;
    os_state.queue_count = 0;
    os_state.window_thread = SDL_CreateThread(window_thread_main, "puppetxr window", nullptr);
    if (os_state.window_thread)
    {
        SDL_WaitSemaphore(os_state.window_ready);
    }
    if (os_state.window_thread && !os_state.window_thread_ok)
    {
        SDL_WaitThread(os_state.window_thread, nullptr);
        os_state.window_thread = nullptr;
    }
    return os_state.window_thread != nullptr;
}

static void set_window_quit (void* user) // NOLINT(misc-const-correctness): SDL_MainThreadCallback
{
    (void)user;
    SDL_SetAtomicInt(&os_state.window_quit, 1);
}

static void stop_window_thread ()
{
    if (os_state.window_thread)
    {
        SDL_RunOnMainThread(set_window_quit, nullptr, true);
        SDL_WaitThread(os_state.window_thread, nullptr);
        os_state.window_thread = nullptr;
    }
    SDL_DestroySemaphore(os_state.window_ready);
    SDL_DestroySemaphore(os_state.call_done);
    SDL_DestroySemaphore(os_state.window_wake);
    SDL_DestroyMutex(os_state.call_mutex);
    SDL_DestroyMutex(os_state.call_slot);
    os_state.call_slot   = nullptr;
    os_state.call_done   = nullptr;
    os_state.window_wake = nullptr;
    os_state.call_mutex  = nullptr;
    SDL_DestroyMutex(os_state.queue_mutex);
    os_memory_release(os_state.queue);
    os_state.window_ready = nullptr;
    os_state.queue_mutex  = nullptr;
    os_state.queue        = nullptr;
    os_state.wake_event   = 0;
}
#endif

#if HAVE_MACOS
static b8 on_main_thread ()
{
    return pthread_main_np() != 0;
}

static void main_queue_create ()
{
    if (os_main_queue.mutex)
    {
        return;
    }
    os_main_queue.done  = SDL_CreateCondition();
    os_main_queue.wake  = SDL_CreateSemaphore(0);
    os_main_queue.mutex = SDL_CreateMutex();
}

// Runs every queued call, oldest first; a call that waits for a mutex serves no others
static void serve_main_calls ()
{
    static b8 serving = false;
    if (!os_main_queue.mutex || serving || !on_main_thread())
    {
        return;
    }
    serving = true;
    SDL_LockMutex(os_main_queue.mutex);
    os_main_queue.stalled      = false;
    os_main_queue.stall_logged = false;
    for (;;)
    {
        MainCall* next = nullptr;
        for (MainCall& call : os_main_queue.calls)
        {
            if (call.state == MainCallState::Queued && (!next || call.order < next->order))
            {
                next = &call;
            }
        }
        if (!next)
        {
            break;
        }
        next->state = MainCallState::Running;
        SDL_UnlockMutex(os_main_queue.mutex);
        next->function(next->user);
        SDL_LockMutex(os_main_queue.mutex);
        next->state = next->posted ? MainCallState::Free : MainCallState::Done;
        SDL_BroadcastCondition(os_main_queue.done);
    }
    SDL_UnlockMutex(os_main_queue.mutex);
    serving = false;
}

static void serve_from_dispatch (void* user) // NOLINT(misc-const-correctness): dispatch_function_t
{
    (void)user;
    SDL_SetAtomicInt(&os_main_queue.dispatched, 0);
    serve_main_calls();
}

// The main thread serves either in its run loop, which the dispatch wakes, or in a mutex wait
static void wake_main_thread ()
{
    SDL_SignalSemaphore(os_main_queue.wake);
    if (SDL_CompareAndSwapAtomicInt(&os_main_queue.dispatched, 0, 1))
    {
        dispatch_async_f(dispatch_get_main_queue(), nullptr, serve_from_dispatch);
    }
}

// Under the queue's mutex
static MainCall* queue_main_call (OS_WindowThreadFunction* function, void* user, b8 posted)
{
    for (MainCall& call : os_main_queue.calls)
    {
        if (call.state == MainCallState::Free)
        {
            call = { .function = function, .user = user, .state = MainCallState::Queued, .posted = posted, .order = os_main_queue.next_order++ };
            return &call;
        }
    }
    if (!os_main_queue.full_logged)
    {
        os_main_queue.full_logged = true;
        LOG_WARNING("%u calls wait for the main thread; dropping more", OS_MAX_MAIN_CALLS);
    }
    return nullptr;
}

// False when the call did not run: the main thread did not start it in time, or is stalled
static b8 run_on_main_thread (OS_WindowThreadFunction* function, void* user, u32 timeout_ms)
{
    SDL_LockMutex(os_main_queue.mutex);
    MainCall* call = os_main_queue.stalled ? nullptr : queue_main_call(function, user, false);
    if (!call)
    {
        SDL_UnlockMutex(os_main_queue.mutex);
        return false;
    }
    wake_main_thread();
    PROF_ZONE("window thread call wait");
    const u64 deadline = os_monotonic_ns() + (u64)timeout_ms * 1000000ull;
    while (call->state != MainCallState::Done)
    {
        const u64 now = os_monotonic_ns();
        if (call->state == MainCallState::Queued && now >= deadline)
        {
            call->state           = MainCallState::Free;
            os_main_queue.stalled = true;
            if (!os_main_queue.stall_logged)
            {
                os_main_queue.stall_logged = true;
                LOG_WARNING("The main thread did not take a window call within %u ms; window calls fail until it comes by (the app blocks it on the thread that calls OpenXR)", timeout_ms);
            }
            SDL_UnlockMutex(os_main_queue.mutex);
            return false;
        }
        // A running call is waited out: it cannot be withdrawn
        const u64 remaining = call->state == MainCallState::Queued ? deadline - now : 100000000ull;
        SDL_WaitConditionTimeout(os_main_queue.done, os_main_queue.mutex, (i32)(remaining / 1000000ull) + 1);
    }
    call->state = MainCallState::Free;
    SDL_UnlockMutex(os_main_queue.mutex);
    return true;
}

// Inside NSApp's running event loop (Unity's editor and players, NSApplicationMain apps) the app
// dispatches the events itself; a pump there runs the app's frames nested in ours
static b8 app_runs_event_loop ()
{
    const CFRunLoopMode mode = CFRunLoopCopyCurrentMode(CFRunLoopGetMain());
    if (!mode)
    {
        return false;
    }
    CFRelease(mode);
    const void* const app = ((void* (*)(void*, SEL))objc_msgSend)((void*)objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    return app && ((signed char (*)(const void*, SEL))objc_msgSend)(app, sel_registerName("isRunning")) != 0;
}
#endif

#if HAVE_MACOS
namespace
{

struct VideoInitCall
{
    b8 ok;
};

} // namespace

static void add_input_monitor ();
static bool SDLCALL note_sdl_input_event (void* user, SDL_Event* event);

static void init_video_now (void* user)
{
    ((VideoInitCall*)user)->ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    if (((VideoInitCall*)user)->ok)
    {
        add_input_monitor();
        SDL_AddEventWatch(note_sdl_input_event, nullptr);
    }
}
#endif

b8 os_initialize (b8 headless)
{
    if (os_state.initialized)
    {
        return true;
    }

    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
    // The click that focuses the mirror window also clicks the panels, instead of only focusing
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    // Looking keeps the cursor where it was instead of moving it to the window's middle
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "0");
#if HAVE_WINDOWS
    // SDL imports IMM32, which user32 then calls for every new window without holding it: our
    // unload unloaded it in an app that does not import it, and a window created after on any
    // thread (OBS's capture hook) crashed with STATUS_FATAL_USER_CALLBACK_EXCEPTION
    HMODULE imm32 = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"imm32.dll", &imm32);
#endif

#if HAVE_WINDOW_THREAD
    if (!headless && !start_window_thread())
    {
        stop_window_thread();
        headless = true;
    }
#elif HAVE_MACOS
    // Cocoa starts on the main thread only, which may be busy or blocked on the caller
    main_queue_create();
    if (!headless)
    {
        VideoInitCall call = {};
        if (!on_main_thread() && !run_on_main_thread(init_video_now, &call, 2000))
        {
            LOG_WARNING("The main thread does not take window calls, continuing headless");
            headless = true;
        }
        else if (on_main_thread())
        {
            init_video_now(&call);
        }
        if (!headless && !call.ok)
        {
            LOG_WARNING("No display available (%s), continuing headless", SDL_GetError());
            headless = true;
        }
    }
#endif
    if (headless && !SDL_Init(SDL_INIT_EVENTS))
    {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    os_state.initialized = true;
    os_state.headless    = headless;
    return true;
}

void os_shutdown ()
{
    if (!os_state.initialized)
    {
        return;
    }
#if HAVE_WINDOW_THREAD
    if (!os_state.headless)
    {
        stop_window_thread();
    }
#endif
    SDL_Quit();
    os_state.initialized   = false;
    os_state.pushed_window = 0;
}

#if HAVE_WINDOW_THREAD
// Runs the waiting call if it is `ticket`'s, any waiting call for 0. SDL's atomics are not built
// with ThreadSanitizer, which then saw no order between the call's fields and its ticket
static void run_window_thread_call (i32 ticket)
{
    SDL_LockMutex(os_state.call_slot);
    const b8                       taken    = os_state.call_ticket && (!ticket || ticket == os_state.call_ticket);
    OS_WindowThreadFunction* const function = os_state.call_function;
    void* const                    user     = os_state.call_user;
    if (taken)
    {
        os_state.call_ticket = 0;
    }
    SDL_UnlockMutex(os_state.call_slot);
    if (taken)
    {
        function(user);
        SDL_SignalSemaphore(os_state.call_done);
    }
}

static void window_thread_call (void* user) // NOLINT(misc-const-correctness): SDL_MainThreadCallback
{
    run_window_thread_call((i32)(intptr_t)user);
}

static b8 on_window_thread ()
{
    return os_state.window_thread && SDL_GetCurrentThreadID() == SDL_GetThreadID(os_state.window_thread);
}
#endif


b8 os_is_window_thread ()
{
    if (os_state.headless || !os_state.initialized)
    {
        return true;
    }
#if HAVE_WINDOW_THREAD
    return !os_state.window_thread || SDL_GetCurrentThreadID() == SDL_GetThreadID(os_state.window_thread);
#else
    return on_main_thread();
#endif
}

// SDL wakes its waiting thread for a queued call only now and then, so a call waited out the
// window thread's 100 ms event wait and apps ran at 10 frames a second while no input came. A
// wake of our own ends its wait.
void os_run_on_window_thread (OS_WindowThreadFunction* function, void* user)
{
    ASSERT(function);
#if HAVE_WINDOW_THREAD
    if (os_state.window_thread && !on_window_thread())
    {
        SDL_LockMutex(os_state.call_mutex);
        os_state.call_tickets = os_state.call_tickets == INT32_MAX ? 1 : os_state.call_tickets + 1;
        const i32 ticket      = os_state.call_tickets;
        SDL_LockMutex(os_state.call_slot);
        ASSERT(!os_state.call_ticket);
        os_state.call_function = function;
        os_state.call_user     = user;
        os_state.call_ticket   = ticket;
        SDL_UnlockMutex(os_state.call_slot);
        // A call SDL did not queue is withdrawn, unless the waiting window thread took it
        b8 withdrawn = false;
        if (!SDL_RunOnMainThread(window_thread_call, (void*)(intptr_t)ticket, false))
        {
            SDL_LockMutex(os_state.call_slot);
            withdrawn = os_state.call_ticket == ticket;
            os_state.call_ticket = withdrawn ? 0 : os_state.call_ticket;
            SDL_UnlockMutex(os_state.call_slot);
        }
        if (!withdrawn)
        {
            // Again until it ran, as a wakeup may come before the thread waits (always in Wine);
            // the window thread may also wait for a mutex this thread holds (os_mutex_lock)
            do
            {
                wake_window_thread();
                if (SDL_GetAtomicInt(&os_state.window_waiting) > 0)
                {
                    SDL_SignalSemaphore(os_state.window_wake);
                }
            } while (!SDL_WaitSemaphoreTimeout(os_state.call_done, 1));
        }
        SDL_UnlockMutex(os_state.call_mutex);
        return;
    }
#elif HAVE_MACOS
    if (!os_is_window_thread() && os_main_queue.mutex)
    {
        run_on_main_thread(function, user, OS_MAIN_THREAD_TIMEOUT_MS);
        return;
    }
#endif
    function(user);
}

#if HAVE_WINDOW_THREAD
namespace
{

struct PostedCall
{
    OS_WindowThreadFunction* function;
    void*                    user;
};

} // namespace

static void posted_call (void* user) // NOLINT(misc-const-correctness): SDL_MainThreadCallback
{
    const PostedCall call = *(const PostedCall*)user;
    os_memory_release(user);
    call.function(call.user);
}
#endif

void os_post_to_window_thread (OS_WindowThreadFunction* function, void* user)
{
    ASSERT(function);
    if (os_is_window_thread())
    {
        function(user);
        return;
    }
#if HAVE_WINDOW_THREAD
    PostedCall* call = (PostedCall*)os_memory_alloc(sizeof(PostedCall));
    *call            = { .function = function, .user = user };
    if (!SDL_RunOnMainThread(posted_call, call, false))
    {
        os_memory_release(call);
        return;
    }
    wake_window_thread();
#elif HAVE_MACOS
    SDL_LockMutex(os_main_queue.mutex);
    const MainCall* call = queue_main_call(function, user, true);
    SDL_UnlockMutex(os_main_queue.mutex);
    if (call)
    {
        wake_main_thread();
    }
#endif
}

#if HAVE_WINDOW_THREAD
static void barrier_call (void* user)
{
    (void)user;
}
#endif

void os_serve_posted (u64 ns)
{
#if HAVE_MACOS
    if (on_main_thread())
    {
        serve_main_calls();
        if (ns > 0)
        {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, (f64)ns * 1e-9, false);
            serve_main_calls();
        }
        return;
    }
#elif HAVE_WINDOW_THREAD
    // Posted calls run in order, so one blocking call after them waits them out
    if (os_state.window_thread && !os_is_window_thread())
    {
        os_run_on_window_thread(barrier_call, nullptr);
    }
#endif
    if (ns > 0)
    {
        os_sleep_ns(ns);
    }
}

void os_release ()
{
    if (os_state.log_file)
    {
        os_file_close(os_state.log_file);
        os_state.log_file = nullptr;
    }
    os_state.log_file_tried = false;
    if (os_state.log_mutex)
    {
        SDL_DestroyMutex(os_state.log_mutex);
        os_state.log_mutex = nullptr;
    }
}

b8 os_is_headless ()
{
    return os_state.headless;
}

//
// Memory
//

void* os_memory_alloc (usize size)
{
    return SDL_calloc(1, size ? size : 1);
}

void* os_memory_realloc (void* pointer, usize size)
{
    return SDL_realloc(pointer, size ? size : 1);
}

void os_memory_release (void* pointer)
{
    SDL_free(pointer);
}

//
// Time and environment
//

u64 os_time_ns ()
{
    return SDL_GetTicksNS();
}

u64 os_monotonic_ns ()
{
#if HAVE_WINDOWS
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return os_native_ticks_to_ns(now.QuadPart);
#else
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000000000ull + (u64)now.tv_nsec;
#endif
}

u64 os_timespec_ns ()
{
#if HAVE_WINDOWS
    // 100 ns intervals since 1601, the same source as the UCRT's timespec_get(TIME_UTC)
    FILETIME now = {};
    GetSystemTimePreciseAsFileTime(&now);
    const u64 intervals = ((u64)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return (intervals - 116444736000000000ull) * 100;
#else
    return os_monotonic_ns();
#endif
}

OS_NativeClock os_native_clock ()
{
    return HAVE_WINDOWS ? OS_NativeClock::PerformanceCounter : OS_NativeClock::Timespec;
}

static i64 native_ticks_per_second ()
{
#if HAVE_WINDOWS
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart > 0 ? frequency.QuadPart : 1;
#else
    return 1000000000;
#endif
}

// Split so that ticks * 1e9 never overflows, whatever the frequency
u64 os_native_ticks_to_ns (i64 ticks)
{
    if (ticks <= 0)
    {
        return 0;
    }
    const u64 frequency = (u64)native_ticks_per_second();
    const u64 count     = (u64)ticks;
    return count / frequency * 1000000000ull + count % frequency * 1000000000ull / frequency;
}

i64 os_ns_to_native_ticks (u64 ns)
{
    const u64 frequency = (u64)native_ticks_per_second();
    return (i64)(ns / 1000000000ull * frequency + ns % 1000000000ull * frequency / 1000000000ull);
}

void os_sleep_ns (u64 ns)
{
    SDL_DelayPrecise(ns);
}

#if HAVE_WINDOWS

// The C runtime's getenv answers in the ANSI code page and sees only its own copy. The process
// environment is read in UTF-16 instead and the UTF-8 value kept in SDL's environment, whose
// strings stay valid until the variable changes.
const char* os_env (const char* name)
{
    wchar_t wide_name[256];
    if (!to_wide(name, wide_name, ARRAY_SIZE(wide_name)))
    {
        return nullptr;
    }
    SDL_Environment* environment = SDL_GetEnvironment();
    wchar_t          wide_value[OS_MAX_PATH];
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableW(wide_name, wide_value, ARRAY_SIZE(wide_value));
    if (length >= ARRAY_SIZE(wide_value))
    {
        LOG_ERROR("Environment variable %s is longer than %u characters", name, (u32)ARRAY_SIZE(wide_value));
        return nullptr;
    }
    if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    {
        SDL_UnsetEnvironmentVariable(environment, name);
        return nullptr;
    }
    wide_value[length] = 0;
    char value[OS_MAX_PATH * 3];
    if (!from_wide(wide_value, value, sizeof(value)))
    {
        return nullptr;
    }
    const char* kept = SDL_GetEnvironmentVariable(environment, name);
    if (!kept || !cstr_equal(kept, value))
    {
        SDL_SetEnvironmentVariable(environment, name, value, true);
        kept = SDL_GetEnvironmentVariable(environment, name);
    }
    return kept;
}

// Both the process environment and the C runtime's copy, which _wputenv_s keeps in step
void os_set_env (const char* name, const char* value)
{
    wchar_t wide_name[256];
    wchar_t wide_value[OS_MAX_PATH];
    if (to_wide(name, wide_name, ARRAY_SIZE(wide_name)) && to_wide(value ? value : "", wide_value, ARRAY_SIZE(wide_value)))
    {
        _wputenv_s(wide_name, wide_value);
        SetEnvironmentVariableW(wide_name, wide_value);
    }
}

void os_unset_env (const char* name)
{
    wchar_t wide_name[256];
    if (to_wide(name, wide_name, ARRAY_SIZE(wide_name)))
    {
        _wputenv_s(wide_name, L"");
        SetEnvironmentVariableW(wide_name, nullptr);
    }
}

#else

const char* os_env (const char* name)
{
    return SDL_getenv_unsafe(name);
}

void os_set_env (const char* name, const char* value)
{
    SDL_setenv_unsafe(name, value, 1);
}

void os_unset_env (const char* name)
{
    SDL_unsetenv_unsafe(name);
}

#endif

//
// Files
//

b8 os_file_exists (const char* path)
{
    if (!path)
    {
        return false;
    }
    SDL_PathInfo info = {};
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

#if HAVE_MACOS
static const char* path_leaf (const char* path)
{
    const char* leaf = path;
    for (const char* c = path; *c; c++)
    {
        if (*c == '/')
        {
            leaf = c + 1;
        }
    }
    return leaf;
}

static const char* image_install_name (u32 index)
{
    const mach_header* header = _dyld_get_image_header(index);
    if (!header || header->magic != MH_MAGIC_64)
    {
        return nullptr;
    }
    const u8* command = (const u8*)header + sizeof(mach_header_64);
    for (u32 i = 0; i < header->ncmds; i++)
    {
        const load_command* load = (const load_command*)command;
        if (load->cmd == LC_ID_DYLIB)
        {
            return (const char*)command + ((const dylib_command*)command)->dylib.name.offset;
        }
        command += load->cmdsize;
    }
    return nullptr;
}

// dyld resolves symlinks, so libvulkan.1.dylib is loaded as libvulkan.1.4.363.dylib; its install
// name keeps the short one. An app may load its library from off dyld's search path.
static const char* loaded_image_path (const char* name)
{
    const b8 bare = !cstr_find(name, "/");
    for (u32 i = 0; i < _dyld_image_count(); i++)
    {
        const char* path = _dyld_get_image_name(i);
        if (!path)
        {
            continue;
        }
        if (cstr_equal(path, name) || (bare && cstr_equal(path_leaf(path), name)))
        {
            return path;
        }
        const char* install_name = image_install_name(i);
        if (install_name && bare && cstr_equal(path_leaf(install_name), name))
        {
            return path;
        }
    }
    return nullptr;
}
#endif

OS_Library* os_library_open (const char* name)
{
    if (!name || !name[0])
    {
        return nullptr;
    }
#if HAVE_WINDOWS
    wchar_t wide[OS_MAX_PATH];
    return to_wide(name, wide, ARRAY_SIZE(wide)) ? (OS_Library*)LoadLibraryW(wide) : nullptr;
#else
#if HAVE_MACOS
    const char* loaded = loaded_image_path(name);
    if (loaded)
    {
        name = loaded;
    }
#endif
    return (OS_Library*)dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

b8 os_library_path (const char* name, char* out_path, usize size)
{
    if (!name || !name[0] || size == 0)
    {
        return false;
    }
    out_path[0] = 0;
#if HAVE_MACOS
    const char* loaded = loaded_image_path(name);
    return loaded && cstr_copy(out_path, size, loaded);
#else
    return false;
#endif
}

#if HAVE_WINDOWS
static const char* const vulkan_loader_names[] = { "vulkan-1.dll" };
#elif HAVE_MACOS
static const char* const vulkan_loader_names[] = { "libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib", "vulkan", "MoltenVK" };
#else
static const char* const vulkan_loader_names[] = { "libvulkan.so.1", "libvulkan.so" };
#endif

// The app's loader rather than whichever the search path finds first: an app bundle carries its own
OS_Library* os_vulkan_loader ()
{
    for (const char* name : vulkan_loader_names)
    {
        if (os_module_loaded(name))
        {
            return os_library_open(name);
        }
    }
    return nullptr;
}

void os_library_close (OS_Library* library)
{
    if (!library)
    {
        return;
    }
#if HAVE_WINDOWS
    FreeLibrary((HMODULE)library);
#else
    dlclose(library);
#endif
}

b8 os_module_loaded (const char* name)
{
    if (!name || !name[0])
    {
        return false;
    }
#if HAVE_WINDOWS
    wchar_t wide[OS_MAX_PATH];
    return to_wide(name, wide, ARRAY_SIZE(wide)) && GetModuleHandleW(wide) != nullptr;
#else
#if HAVE_MACOS
    if (loaded_image_path(name))
    {
        return true;
    }
#endif
    void* library = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
    if (!library)
    {
        return false;
    }
    dlclose(library);
    return true;
#endif
}

void* os_library_symbol (OS_Library* library, const char* name)
{
    if (!name || !name[0])
    {
        return nullptr;
    }
#if HAVE_WINDOWS
    if (library)
    {
        return (void*)GetProcAddress((HMODULE)library, name);
    }
    // Windows has no global symbol namespace; every loaded module is asked in turn
    HMODULE modules[1024];
    DWORD   needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
    {
        return nullptr;
    }
    const DWORD count = needed / sizeof(HMODULE) < ARRAY_SIZE(modules) ? needed / (DWORD)sizeof(HMODULE) : (DWORD)ARRAY_SIZE(modules);
    for (DWORD i = 0; i < count; i++)
    {
        void* symbol = (void*)GetProcAddress(modules[i], name);
        if (symbol)
        {
            return symbol;
        }
    }
    return nullptr;
#else
    return dlsym(library ? (void*)library : RTLD_DEFAULT, name);
#endif
}

b8 os_module_directory (char* out_dir, usize out_size)
{
    char path[OS_MAX_PATH] = {};
#if HAVE_WINDOWS
    HMODULE module = nullptr;
    wchar_t wide[OS_MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&os_state, &module))
    {
        return false;
    }
    const DWORD length = GetModuleFileNameW(module, wide, ARRAY_SIZE(wide));
    if (length == 0 || length >= ARRAY_SIZE(wide) || !from_wide(wide, path, sizeof(path)))
    {
        return false;
    }
#else
    Dl_info info = {};
    if (!dladdr((const void*)&os_module_directory, &info) || !info.dli_fname || !cstr_copy(path, sizeof(path), info.dli_fname))
    {
        return false;
    }
#endif
    usize end = 0;
    for (usize i = 0; path[i]; i++)
    {
        if (path[i] == '/' || path[i] == '\\')
        {
            end = i;
        }
    }
    if (end == 0 || end + 1 > out_size)
    {
        return false;
    }
    path[end] = 0;
    return cstr_copy(out_dir, out_size, path);
}

b8 os_directory_exists (const char* path)
{
    if (!path)
    {
        return false;
    }
    SDL_PathInfo info = {};
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

b8 os_list_directory (const char* path, void (*callback)(const char* name, void* user), void* user)
{
    ASSERT(callback);
    if (!os_directory_exists(path))
    {
        return false;
    }
    OS_ListContext context = { .callback = callback, .user = user };
    return SDL_EnumerateDirectory(path, list_entry, &context);
}

// SDL passes the directory with its trailing separator
static SDL_EnumerationResult SDLCALL delete_entry (void* user, const char* dirname, const char* fname) // NOLINT(misc-const-correctness): SDL_EnumerateDirectoryCallback
{
    (void)user;
    char path[OS_MAX_PATH];
    if (SDL_snprintf(path, sizeof(path), "%s%s", dirname, fname) >= (int)sizeof(path))
    {
        return SDL_ENUM_FAILURE;
    }
    return os_delete_tree(path) ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
}

b8 os_delete_tree (const char* path)
{
    SDL_PathInfo info = {};
    if (!path || !path[0])
    {
        return false;
    }
    if (!SDL_GetPathInfo(path, &info))
    {
        return true;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY && !SDL_EnumerateDirectory(path, delete_entry, nullptr))
    {
        return false;
    }
    return SDL_RemovePath(path);
}

b8 os_read_file (const char* path, u8** out_data, usize* out_size)
{
    ASSERT(out_data && out_size);
    *out_data = nullptr;
    *out_size = 0;

    if (!path)
    {
        return false;
    }

#if HAVE_WINDOWS
    // Shares writing and deletion, so a file another handle writes (a recording, a lock file)
    // can be read as on POSIX
    wchar_t wide[OS_MAX_PATH];
    if (!to_wide(path, wide, ARRAY_SIZE(wide)))
    {
        return false;
    }
    const HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER length = {};
    u8*           data   = nullptr;
    usize         done   = 0;
    if (GetFileSizeEx(file, &length) && length.QuadPart >= 0)
    {
        const usize size = (usize)length.QuadPart;
        data             = (u8*)os_memory_alloc(size + 1); // terminated, as SDL_LoadFile does
        while (done < size)
        {
            const usize rest = size - done;
            const DWORD part = rest > 0x40000000 ? 0x40000000 : (DWORD)rest;
            DWORD       read = 0;
            if (!ReadFile(file, data + done, part, &read, nullptr) || read == 0)
            {
                break;
            }
            done += read;
        }
        if (done != size)
        {
            os_memory_release(data);
            data = nullptr;
        }
    }
    CloseHandle(file);
    if (!data)
    {
        return false;
    }
    *out_data = data;
    *out_size = done;
    return true;
#else
    usize size = 0;
    void* data = SDL_LoadFile(path, &size);
    if (!data)
    {
        return false;
    }

    *out_data = (u8*)data;
    *out_size = size;
    return true;
#endif
}

b8 os_write_file (const char* path, const void* data, usize size)
{
    if (!path)
    {
        return false;
    }
    return SDL_SaveFile(path, data, size);
}

b8 os_write_file_atomic (const char* path, const void* data, usize size)
{
    char temporary[OS_MAX_PATH];
    if (!path || cstr_format(temporary, sizeof(temporary), "%s.tmp", path) >= sizeof(temporary))
    {
        return false;
    }
    OS_File* file = os_file_create(temporary);
    if (!file)
    {
        return false;
    }
    const b8 written = os_file_write(file, data, size) && os_file_flush(file);
    if (!os_file_close(file) || !written || !SDL_RenamePath(temporary, path))
    {
        os_delete_file(temporary);
        return false;
    }
    return true;
}

b8 os_create_directories (const char* path)
{
    char partial[OS_MAX_PATH] = {};
    if (!path || !path[0] || !cstr_copy(partial, sizeof(partial), path))
    {
        return false;
    }
    for (char* at = partial + 1; *at; at++)
    {
        if (*at == '/' || *at == '\\')
        {
            const char separator = *at;
            *at = 0;
            os_create_directory(partial);
            *at = separator;
        }
    }
    return os_create_directory(partial);
}

//
// Window and events
//

static OS_Key key_from_scancode (SDL_Scancode scancode)
{
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
    {
        return (OS_Key)((u32)OS_Key::A + (u32)(scancode - SDL_SCANCODE_A));
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
    {
        return (OS_Key)((u32)OS_Key::Digit1 + (u32)(scancode - SDL_SCANCODE_1));
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
    {
        return (OS_Key)((u32)OS_Key::F1 + (u32)(scancode - SDL_SCANCODE_F1));
    }
    switch (scancode)
    {
    case SDL_SCANCODE_0:
        return OS_Key::Digit0;
    case SDL_SCANCODE_SPACE:
        return OS_Key::Space;
    case SDL_SCANCODE_ESCAPE:
        return OS_Key::Escape;
    case SDL_SCANCODE_RETURN:
        return OS_Key::Enter;
    case SDL_SCANCODE_TAB:
        return OS_Key::Tab;
    case SDL_SCANCODE_BACKSPACE:
        return OS_Key::Backspace;
    case SDL_SCANCODE_LEFT:
        return OS_Key::ArrowLeft;
    case SDL_SCANCODE_RIGHT:
        return OS_Key::ArrowRight;
    case SDL_SCANCODE_UP:
        return OS_Key::ArrowUp;
    case SDL_SCANCODE_DOWN:
        return OS_Key::ArrowDown;
    case SDL_SCANCODE_LSHIFT:
        return OS_Key::LeftShift;
    case SDL_SCANCODE_LCTRL:
        return OS_Key::LeftCtrl;
    case SDL_SCANCODE_LALT:
        return OS_Key::LeftAlt;
    case SDL_SCANCODE_RSHIFT:
        return OS_Key::RightShift;
    case SDL_SCANCODE_RCTRL:
        return OS_Key::RightCtrl;
    case SDL_SCANCODE_RALT:
        return OS_Key::RightAlt;
    case SDL_SCANCODE_GRAVE:
        return OS_Key::Grave;
    case SDL_SCANCODE_LEFTBRACKET:
        return OS_Key::LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET:
        return OS_Key::RightBracket;
    default:
        return OS_Key::None;
    }
}

static SDL_Scancode scancode_from_key (OS_Key key)
{
    if (key == OS_Key::None)
    {
        return SDL_SCANCODE_UNKNOWN;
    }
    // Scancodes below A are none or reserved, never a key
    for (u32 scancode = SDL_SCANCODE_A; scancode < SDL_SCANCODE_COUNT; scancode++)
    {
        if (key_from_scancode((SDL_Scancode)scancode) == key)
        {
            return (SDL_Scancode)scancode;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

static OS_Key key_from_mouse_button (u8 button)
{
    switch (button)
    {
    case SDL_BUTTON_LEFT:
        return OS_Key::MouseLeft;
    case SDL_BUTTON_RIGHT:
        return OS_Key::MouseRight;
    case SDL_BUTTON_MIDDLE:
        return OS_Key::MouseMiddle;
    default:
        return OS_Key::None;
    }
}

// Every window function runs through os_run_on_window_thread (see OS_QueuedEvent), the
// arguments and results in a struct of the call

const char* os_video_driver ()
{
    return os_state.headless ? nullptr : SDL_GetCurrentVideoDriver();
}

namespace
{

struct VideoDriverCall
{
    const char* driver;
    b8          result;
};

struct CreateWindowCall
{
    const char* title;
    u32         width;
    u32         height;
    b8          vulkan;
    u32         x11_visual;
    b8          hidden;
    OS_Window*  result;
};

struct NativeHandlesCall
{
    OS_Window*       window;
    OS_WindowNative* out;
    b8               result;
};

struct WindowSizeCall
{
    OS_Window* window;
    i32        width;
    i32        height;
};

struct WindowDensityCall
{
    OS_Window* window;
    f32        density;
};

struct VulkanExtensionsCall
{
    const char* const* names;
    u32                count;
};

struct VulkanSurfaceCall
{
    OS_Window*  window;
    void*       vk_instance;
    const void* vk_get_instance_proc_addr;
    u64         surface;
    b8          result;
};

struct RelativeMouseCall
{
    OS_Window* window;
    b8         relative;
};

struct MaximizedCall
{
    OS_Window* window;
    b8         maximized;
};

struct KeepCursorCall
{
    OS_Window* window;
    b8         held;
};

struct WindowCursorCall
{
    OS_Window* window;
    f32        x;
    f32        y;
    b8         result;
};

} // namespace

static void set_video_driver_now (void* user)
{
    VideoDriverCall& call    = *(VideoDriverCall*)user;
    const char*      current = SDL_GetCurrentVideoDriver();
    if (current && cstr_equal(current, call.driver))
    {
        call.result = true;
        return;
    }
    char previous[32] = {};
    cstr_copy(previous, sizeof(previous), current ? current : "");
    // Override priority, as SDL_VIDEODRIVER in the environment beats a plain hint
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, call.driver, SDL_HINT_OVERRIDE);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        call.result = true;
        return;
    }
    LOG_WARNING("Video driver %s is not available (%s)", call.driver, SDL_GetError());
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, previous[0] ? previous : nullptr, SDL_HINT_OVERRIDE);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        LOG_ERROR("Cannot restart video on %s: %s", previous, SDL_GetError());
    }
    call.result = false;
}

b8 os_set_video_driver (const char* driver)
{
    if (os_state.headless || !driver || !driver[0])
    {
        return false;
    }
    VideoDriverCall call = { .driver = driver, .result = false };
    os_run_on_window_thread(set_video_driver_now, &call);
    return call.result;
}

#if HAVE_MACOS
// SDL's Cocoa backend takes key events, and mouse moves in relative mode, only in its own pump
// or inside its own NSApp class. Unity's editor and Godot make NSApp of their own class and
// dispatch events from their own loop, Unity's editor also inside SDL's pump: released keys stayed
// held and looking did not turn. A local monitor, which fires inside -[NSApplication sendEvent:],
// hands the events of our windows to SDL's handlers, except the one SDL's own pump just handled.
// SDL internals, reached as SDL_cocoaevents.m does
extern "C" void* SDL_GetVideoDevice    ();
extern "C" void  Cocoa_HandleKeyEvent  (void* video, void* event);
extern "C" void  Cocoa_HandleMouseEvent(void* video, void* event);

#define OS_MAX_COCOA_WINDOWS 8

enum : u64
{
    NS_EVENT_MOUSE_MOVED          = 5,
    NS_EVENT_LEFT_MOUSE_DRAGGED   = 6,
    NS_EVENT_RIGHT_MOUSE_DRAGGED  = 7,
    NS_EVENT_KEY_DOWN             = 10,
    NS_EVENT_KEY_UP               = 11,
    NS_EVENT_FLAGS_CHANGED        = 12,
    NS_EVENT_OTHER_MOUSE_DRAGGED  = 27,
};

namespace
{

struct CocoaInput
{
    const void* windows[OS_MAX_COCOA_WINDOWS]; // the NSWindows of our SDL windows
    void*       monitor;
    b8          forwarding;  // our own call into SDL's handlers, which the watch must not note
    b8          key_noted;   // SDL's pump handled this key event before the monitor saw it
    u32         key_raw;
    b8          key_down;
    b8          motion_noted;
    f32         motion_x;
    f32         motion_y;
};

} // namespace

static CocoaInput os_cocoa_input = {};

static u64 ns_event_type (const void* event)
{
    return ((u64 (*)(const void*, SEL))objc_msgSend)(event, sel_registerName("type"));
}

static b8 our_window_event (const void* event)
{
    const void* window = ((void* (*)(const void*, SEL))objc_msgSend)(event, sel_registerName("window"));
    for (const void* ours : os_cocoa_input.windows)
    {
        if (window && window == ours)
        {
            return true;
        }
    }
    return false;
}

// SDL swaps the two keys left of 1 and Z on ISO keyboards (scancodes_darwin.h)
static b8 same_key (u32 raw, u32 key_code)
{
    return raw == key_code || ((key_code == 10 || key_code == 50) && raw == 60 - key_code);
}

static void* handle_app_event (void* event)
{
    void* const video = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetVideoDevice() : nullptr;
    if (!video || !event || !our_window_event(event))
    {
        return event;
    }
    const u64 type = ns_event_type(event);
    if (type == NS_EVENT_KEY_DOWN || type == NS_EVENT_KEY_UP)
    {
        const u32 key_code = ((u16 (*)(void*, SEL))objc_msgSend)(event, sel_registerName("keyCode"));
        const b8  skip     = os_cocoa_input.key_noted && same_key(os_cocoa_input.key_raw, key_code) && os_cocoa_input.key_down == (type == NS_EVENT_KEY_DOWN);
        os_cocoa_input.key_noted = false;
        if (skip)
        {
            return event;
        }
        os_cocoa_input.forwarding = true;
        Cocoa_HandleKeyEvent(video, event);
        os_cocoa_input.forwarding = false;
        return event;
    }
    // SDL compares modifiers with its own state, so a second report changes nothing
    if (type == NS_EVENT_FLAGS_CHANGED)
    {
        os_cocoa_input.forwarding = true;
        Cocoa_HandleKeyEvent(video, event);
        os_cocoa_input.forwarding = false;
        return event;
    }
    const f32 dx   = (f32)((f64 (*)(void*, SEL))objc_msgSend)(event, sel_registerName("deltaX"));
    const f32 dy   = (f32)((f64 (*)(void*, SEL))objc_msgSend)(event, sel_registerName("deltaY"));
    const b8  skip = os_cocoa_input.motion_noted && os_cocoa_input.motion_x == dx && os_cocoa_input.motion_y == dy;
    os_cocoa_input.motion_noted = false;
    if (!skip)
    {
        os_cocoa_input.forwarding = true;
        Cocoa_HandleMouseEvent(video, event);
        os_cocoa_input.forwarding = false;
    }
    return event;
}

// Notes SDL's last key event and relative move made on the main thread, for handle_app_event
static bool SDLCALL note_sdl_input_event (void* user, SDL_Event* event) // NOLINT(misc-const-correctness): SDL_EventFilter
{
    (void)user;
    if (os_cocoa_input.forwarding || !on_main_thread())
    {
        return true;
    }
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP)
    {
        os_cocoa_input.key_noted = true;
        os_cocoa_input.key_raw   = event->key.raw;
        os_cocoa_input.key_down  = event->type == SDL_EVENT_KEY_DOWN;
    }
    else if (event->type == SDL_EVENT_MOUSE_MOTION)
    {
        os_cocoa_input.motion_noted = true;
        os_cocoa_input.motion_x     = event->motion.xrel;
        os_cocoa_input.motion_y     = event->motion.yrel;
    }
    return true;
}

// Once per process on the main thread; the monitor stays, and does nothing without SDL's video
static void add_input_monitor ()
{
#if defined(__BLOCKS__)
    if (os_cocoa_input.monitor)
    {
        return;
    }
    const u64 mask = (1ull << NS_EVENT_KEY_DOWN) | (1ull << NS_EVENT_KEY_UP) | (1ull << NS_EVENT_FLAGS_CHANGED) | (1ull << NS_EVENT_MOUSE_MOVED) |
                     (1ull << NS_EVENT_LEFT_MOUSE_DRAGGED) | (1ull << NS_EVENT_RIGHT_MOUSE_DRAGGED) | (1ull << NS_EVENT_OTHER_MOUSE_DRAGGED);
    void* (^const handler)(void*) = ^void*(void* event) { return handle_app_event(event); };
    os_cocoa_input.monitor = ((void* (*)(void*, SEL, u64, void*))objc_msgSend)((void*)objc_getClass("NSEvent"), sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"), mask, (void*)handler);
    if (os_cocoa_input.monitor)
    {
        ((void* (*)(void*, SEL))objc_msgSend)(os_cocoa_input.monitor, sel_registerName("retain"));
    }
#endif
}

static void track_cocoa_window (SDL_Window* window, b8 add)
{
    const void* ns_window = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    for (const void*& slot : os_cocoa_input.windows)
    {
        if (add ? slot == nullptr : slot == ns_window)
        {
            slot = add ? ns_window : nullptr;
            return;
        }
    }
}
#endif

static void create_window_now (void* user)
{
    CreateWindowCall& call       = *(CreateWindowCall*)user;
    char              visual[16] = {};
    if (call.x11_visual)
    {
        cstr_format(visual, sizeof(visual), "%u", call.x11_visual);
    }
    SDL_SetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID, call.x11_visual ? visual : nullptr);
#if HAVE_MACOS
    for (const char* name : vulkan_loader_names)
    {
        char path[OS_MAX_PATH];
        if (call.vulkan && os_library_path(name, path, sizeof(path)))
        {
            SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, path);
            break;
        }
    }
#endif
    // Every pixel of a Retina display rather than a scaled-up quarter; os_window_size is in pixels
    const SDL_WindowFlags flags  = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (call.vulkan ? SDL_WINDOW_VULKAN : 0) | (call.hidden ? SDL_WINDOW_HIDDEN : 0);
    SDL_Window*           window = SDL_CreateWindow(call.title ? call.title : "puppetxr", (i32)call.width, (i32)call.height, flags);
    // A Metal or D3D app may have no Vulkan loader: the window opens without Vulkan and only a
    // Vulkan app's frames cannot show in it
    if (!window && call.vulkan)
    {
        LOG_INFO("No Vulkan for the window (%s); it opens without", SDL_GetError());
        window = SDL_CreateWindow(call.title ? call.title : "puppetxr", (i32)call.width, (i32)call.height, flags & ~SDL_WINDOW_VULKAN);
    }
    SDL_SetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID, nullptr);
    if (!window)
    {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
    }
#if HAVE_MACOS
    if (window)
    {
        track_cocoa_window(window, true);
    }
#endif
    call.result = (OS_Window*)window;
}

OS_Window* os_create_window (const char* title, u32 width, u32 height, b8 vulkan, u32 x11_visual, b8 hidden)
{
    if (os_state.headless)
    {
        LOG_ERROR("Cannot create a window while headless");
        return nullptr;
    }
    CreateWindowCall call = { .title = title, .width = width, .height = height, .vulkan = vulkan, .x11_visual = x11_visual, .hidden = hidden, .result = nullptr };
    os_run_on_window_thread(create_window_now, &call);
    return call.result;
}

#define OS_METAL_VIEW_PROPERTY "puppetxr.metal_view"

static void destroy_window_now (void* user)
{
    SDL_Window* window = (SDL_Window*)user;
#if HAVE_MACOS
    track_cocoa_window(window, false);
#endif
    // NOLINTNEXTLINE(misc-const-correctness): SDL_Metal_DestroyView takes a mutable view
    SDL_MetalView view = SDL_GetPointerProperty(SDL_GetWindowProperties(window), OS_METAL_VIEW_PROPERTY, nullptr);
    if (view)
    {
        SDL_Metal_DestroyView(view);
    }
    SDL_DestroyWindow(window);
}

namespace
{

struct MetalLayerCall
{
    OS_Window* window;
    void*      layer;
};

} // namespace

// One view per window, made on first use and kept with the window
static void window_metal_layer_now (void* user)
{
    MetalLayerCall& call   = *(MetalLayerCall*)user;
    SDL_Window*     window = (SDL_Window*)call.window;
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    SDL_MetalView          view       = SDL_GetPointerProperty(properties, OS_METAL_VIEW_PROPERTY, nullptr);
    if (!view)
    {
        view = SDL_Metal_CreateView(window);
        if (!view)
        {
            LOG_WARNING("No Metal view for the window (%s); compositing offscreen only", SDL_GetError());
            return;
        }
        SDL_SetPointerProperty(properties, OS_METAL_VIEW_PROPERTY, view);
    }
    call.layer = SDL_Metal_GetLayer(view);
}

void* os_window_metal_layer (OS_Window* window)
{
    MetalLayerCall call = { .window = window, .layer = nullptr };
    if (window && HAVE_MACOS)
    {
        os_run_on_window_thread(window_metal_layer_now, &call);
    }
    return call.layer;
}

void os_destroy_window (OS_Window* window)
{
    if (window)
    {
        os_run_on_window_thread(destroy_window_now, window);
    }
}

void* os_window_native (OS_Window* window)
{
    return window;
}

static void window_native_handles_now (void* user)
{
    NativeHandlesCall&     call       = *(NativeHandlesCall*)user;
    OS_WindowNative&       out        = *call.out;
    const SDL_PropertiesID properties = SDL_GetWindowProperties((SDL_Window*)call.window);
    const char*            driver     = SDL_GetCurrentVideoDriver();
    if (driver && cstr_equal(driver, "x11"))
    {
        out.window  = (u64)SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        out.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    }
    else if (driver && cstr_equal(driver, "wayland"))
    {
        out.window  = (u64)(uintptr_t)SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        out.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        out.wayland = true;
    }
    else if (driver && cstr_equal(driver, "windows"))
    {
        out.window  = (u64)(uintptr_t)SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        out.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HDC_POINTER, nullptr);
    }
    call.result = out.window != 0 && out.display != nullptr;
}

b8 os_window_native_handles (OS_Window* window, OS_WindowNative& out_native)
{
    out_native = {};
    if (!window)
    {
        return false;
    }
    NativeHandlesCall call = { .window = window, .out = &out_native, .result = false };
    os_run_on_window_thread(window_native_handles_now, &call);
    return call.result;
}

static void window_size_now (void* user)
{
    WindowSizeCall& call = *(WindowSizeCall*)user;
    SDL_GetWindowSizeInPixels((SDL_Window*)call.window, &call.width, &call.height);
}

void os_window_size (OS_Window* window, u32& out_width, u32& out_height)
{
    WindowSizeCall call = { .window = window, .width = 0, .height = 0 };
    if (window)
    {
        os_run_on_window_thread(window_size_now, &call);
    }
    out_width  = call.width > 0 ? (u32)call.width : 0;
    out_height = call.height > 0 ? (u32)call.height : 0;
}

static void window_display_density_now (void* user)
{
    WindowDensityCall& call = *(WindowDensityCall*)user;
    call.density            = SDL_GetWindowPixelDensity((SDL_Window*)call.window);
}

f32 os_window_display_density (OS_Window* window)
{
    WindowDensityCall call = { .window = window, .density = 0.0f };
    if (window)
    {
        os_run_on_window_thread(window_display_density_now, &call);
    }
    return call.density > 0.0f ? call.density : 1.0f;
}

static void window_refresh_rate_now (void* user)
{
    WindowDensityCall&         call = *(WindowDensityCall*)user;
    const SDL_DisplayMode*     mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow((SDL_Window*)call.window));
    call.density                    = mode ? mode->refresh_rate : 0.0f;
}

f32 os_window_refresh_rate (OS_Window* window)
{
    WindowDensityCall call = { .window = window, .density = 0.0f };
    if (window)
    {
        os_run_on_window_thread(window_refresh_rate_now, &call);
    }
    return call.density > 0.0f ? call.density : 0.0f;
}

static void window_vulkan_extensions_now (void* user)
{
    VulkanExtensionsCall& call = *(VulkanExtensionsCall*)user;
    call.names                 = SDL_Vulkan_GetInstanceExtensions(&call.count);
}

b8 os_window_vulkan_extensions (const char* const*& out_names, u32& out_count)
{
    VulkanExtensionsCall call = { .names = nullptr, .count = 0 };
    os_run_on_window_thread(window_vulkan_extensions_now, &call);
    out_names = call.names;
    out_count = call.names ? call.count : 0;
    return call.names != nullptr;
}

static void window_create_vulkan_surface_now (void* user)
{
    VulkanSurfaceCall& call = *(VulkanSurfaceCall*)user;
    if (call.vk_get_instance_proc_addr && !SDL_Vulkan_LoadLibrary(nullptr))
    {
        LOG_WARNING("SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
    }
    VkSurfaceKHR surface = {};
    if (!SDL_Vulkan_CreateSurface((SDL_Window*)call.window, (VkInstance)call.vk_instance, nullptr, &surface))
    {
        LOG_ERROR("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return;
    }
    call.surface = (u64)surface;
    call.result  = true;
}

b8 os_window_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface)
{
    out_surface = 0;
    if (!window || !vk_instance)
    {
        return false;
    }
    VulkanSurfaceCall call = { .window = window, .vk_instance = vk_instance, .vk_get_instance_proc_addr = vk_get_instance_proc_addr, .surface = 0, .result = false };
    os_run_on_window_thread(window_create_vulkan_surface_now, &call);
    out_surface = call.surface;
    return call.result;
}

static void window_destroy_vulkan_surface_now (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    const VulkanSurfaceCall& call = *(const VulkanSurfaceCall*)user;
    SDL_Vulkan_DestroySurface((VkInstance)call.vk_instance, (VkSurfaceKHR)call.surface, nullptr);
}

void os_window_destroy_vulkan_surface (void* vk_instance, u64 surface)
{
    if (vk_instance && surface)
    {
        VulkanSurfaceCall call = { .window = nullptr, .vk_instance = vk_instance, .vk_get_instance_proc_addr = nullptr, .surface = surface, .result = false };
        os_run_on_window_thread(window_destroy_vulkan_surface_now, &call);
    }
}

static void window_set_relative_mouse_now (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    const RelativeMouseCall& call   = *(const RelativeMouseCall*)user;
    SDL_Window*              window = (SDL_Window*)call.window;
    // The cursor comes back where looking began, not where relative mode left it
    if (call.relative && !SDL_GetWindowRelativeMouseMode(window))
    {
        SDL_GetGlobalMouseState(&os_state.relative_x, &os_state.relative_y);
        os_state.relative_saved = true;
    }
    SDL_SetWindowRelativeMouseMode(window, call.relative);
    if (!call.relative && os_state.relative_saved)
    {
        SDL_WarpMouseGlobal(os_state.relative_x, os_state.relative_y);
        os_state.relative_saved = false;
    }
}

void os_window_set_relative_mouse (OS_Window* window, b8 relative)
{
    if (window)
    {
        RelativeMouseCall call = { .window = window, .relative = relative };
        os_run_on_window_thread(window_set_relative_mouse_now, &call);
    }
}

#if HAVE_MACOS
// SDL maximizes with -[NSWindow zoom:], which animates for some 240 ms on the thread showing the
// window; the window is given the screen's visible frame without the animation instead, and a
// hidden window gets it when shown
#define OS_ZOOM_PROPERTY "puppetxr.zoom_when_shown"

namespace
{

struct NSRectValue
{
    f64 x;
    f64 y;
    f64 width;
    f64 height;
};

} // namespace

static void* cocoa_window (SDL_Window* window)
{
    return SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
}

static void zoom_window (SDL_Window* window)
{
    const void* const ns_window = cocoa_window(window);
    const void*       screen    = ns_window ? ((void* (*)(const void*, SEL))objc_msgSend)(ns_window, sel_registerName("screen")) : nullptr;
    if (!screen)
    {
        screen = ((void* (*)(const void*, SEL))objc_msgSend)((const void*)objc_getClass("NSScreen"), sel_registerName("mainScreen"));
    }
    if (!ns_window || !screen)
    {
        return;
    }
    NSRectValue frame = {};
#if defined(__x86_64__)
    ((void (*)(NSRectValue*, const void*, SEL))objc_msgSend_stret)(&frame, screen, sel_registerName("visibleFrame"));
#else
    frame = ((NSRectValue (*)(const void*, SEL))objc_msgSend)(screen, sel_registerName("visibleFrame"));
#endif
    ((void (*)(const void*, SEL, NSRectValue, signed char, signed char))objc_msgSend)(ns_window, sel_registerName("setFrame:display:animate:"), frame, 1, 0);
}
#endif

static void window_maximize_now (void* user)
{
    MaximizedCall& call   = *(MaximizedCall*)user;
    SDL_Window*    window = (SDL_Window*)call.window;
#if HAVE_MACOS
    if (cocoa_window(window))
    {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN)
        {
            SDL_SetBooleanProperty(SDL_GetWindowProperties(window), OS_ZOOM_PROPERTY, true);
        }
        else
        {
            zoom_window(window);
        }
        call.maximized = true;
        return;
    }
#endif
    call.maximized = SDL_MaximizeWindow(window);
    SDL_SyncWindow(window);
}

// SDL learns of a zoom only from the resize of a visible window, so Cocoa is asked
static void window_maximized_now (void* user)
{
    MaximizedCall& call   = *(MaximizedCall*)user;
    SDL_Window*    window = (SDL_Window*)call.window;
#if HAVE_MACOS
    const void* const ns_window = cocoa_window(window);
    if (ns_window)
    {
        call.maximized = SDL_GetBooleanProperty(SDL_GetWindowProperties(window), OS_ZOOM_PROPERTY, false) || ((signed char (*)(const void*, SEL))objc_msgSend)(ns_window, sel_registerName("isZoomed")) != 0;
        return;
    }
#endif
    call.maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
}

b8 os_window_maximize (OS_Window* window)
{
    MaximizedCall call = { .window = window, .maximized = false };
    if (window)
    {
        os_run_on_window_thread(window_maximize_now, &call);
    }
    return call.maximized;
}

b8 os_window_maximized (OS_Window* window)
{
    MaximizedCall call = { .window = window, .maximized = false };
    if (window)
    {
        os_run_on_window_thread(window_maximized_now, &call);
    }
    return call.maximized;
}

static void window_cursor_now (void* user) // NOLINT(misc-const-correctness): os_run_on_window_thread callback
{
#if HAVE_WINDOWS
    WindowCursorCall& call   = *(WindowCursorCall*)user;
    SDL_Window*       window = (SDL_Window*)call.window;
    HWND              hwnd   = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    POINT             cursor = {};
    if (!hwnd || SDL_GetWindowID(window) == os_state.pushed_window || SDL_GetWindowRelativeMouseMode(window) || !GetCursorPos(&cursor) || WindowFromPoint(cursor) != hwnd || !ScreenToClient(hwnd, &cursor))
    {
        return;
    }
    call.x      = (f32)cursor.x;
    call.y      = (f32)cursor.y;
    call.result = true;
#else
    (void)user;
#endif
}

static b8 rect_inside (OS_ScreenRect inner, OS_ScreenRect outer)
{
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

static void window_keep_cursor_now (void* user) // NOLINT(misc-const-correctness): os_run_on_window_thread callback
{
    const KeepCursorCall& call = *(const KeepCursorCall*)user;
#if HAVE_WINDOWS
    HWND  hwnd   = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window*)call.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    RECT  clip   = {};
    RECT  client = {};
    POINT corner = {};
    if (!hwnd || !GetClipCursor(&clip) || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &corner))
    {
        return;
    }
    OffsetRect(&client, corner.x, corner.y);
    const i32           desktop_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const i32           desktop_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const OS_ScreenRect desktop   = { desktop_x, desktop_y, desktop_x + GetSystemMetrics(SM_CXVIRTUALSCREEN), desktop_y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    switch (os_cursor_clip_action(call.held, GetForegroundWindow() == hwnd, { clip.left, clip.top, clip.right, clip.bottom }, { client.left, client.top, client.right, client.bottom }, desktop))
    {
    case OS_ClipAction::Clip:
        ClipCursor(&client);
        break;
    case OS_ClipAction::Free:
        ClipCursor(nullptr);
        break;
    case OS_ClipAction::Keep:
        break;
    }
#else
    (void)call;
#endif
}

void os_window_keep_cursor (OS_Window* window, b8 held)
{
    KeepCursorCall call = { .window = window, .held = held };
    if (window)
    {
        os_run_on_window_thread(window_keep_cursor_now, &call);
    }
}

// Held, a clip inside the client area is SDL's relative mode and stays; any other was set by
// another window and the cursor would leave. Not held, a clip inside the client area is left over
// from holding, and SDL lifts only its own; another window's goes while this one is in front, as
// it would otherwise cage the cursor in a window the user is not at
OS_ClipAction os_cursor_clip_action (b8 held, b8 foreground, OS_ScreenRect clip, OS_ScreenRect client, OS_ScreenRect desktop)
{
    if (held)
    {
        return rect_inside(clip, client) ? OS_ClipAction::Keep : OS_ClipAction::Clip;
    }
    if (rect_inside(desktop, clip))
    {
        return OS_ClipAction::Keep;
    }
    return rect_inside(clip, client) || foreground ? OS_ClipAction::Free : OS_ClipAction::Keep;
}

// Windows may keep the moves of a mouse from the window: in Unity's editor, whose input takes them,
// the UI saw no move until a click. A window that got pushed mouse events has none: a real cursor
// resting over it would move the UI away from where a test or puppetxrctl clicks
b8 os_window_cursor (OS_Window* window, f32& out_x, f32& out_y)
{
    WindowCursorCall call = { .window = window, .x = 0.0f, .y = 0.0f, .result = false };
    if (window)
    {
        os_run_on_window_thread(window_cursor_now, &call);
    }
    out_x = call.x;
    out_y = call.y;
    return call.result;
}

// The app's own windows, as an editor's play window, open before the first frame shows this one;
// raised, it takes the keyboard and mouse from them. Windows grants that only while the process
// is in the foreground, as an editor is when its play button is clicked.
static void window_show_now (void* user)
{
    SDL_Window* window = (SDL_Window*)user;
    SDL_ShowWindow(window);
#if HAVE_MACOS
    if (SDL_GetBooleanProperty(SDL_GetWindowProperties(window), OS_ZOOM_PROPERTY, false))
    {
        SDL_SetBooleanProperty(SDL_GetWindowProperties(window), OS_ZOOM_PROPERTY, false);
        zoom_window(window);
    }
#endif
    SDL_RaiseWindow(window);
}

void os_window_show (OS_Window* window)
{
    if (window)
    {
        os_run_on_window_thread(window_show_now, window);
    }
}

static void window_hide_now (void* user)
{
    SDL_HideWindow((SDL_Window*)user);
}

void os_window_hide (OS_Window* window)
{
    if (window)
    {
        os_run_on_window_thread(window_hide_now, window);
    }
}

static void window_shown_now (void* user)
{
    MaximizedCall& call = *(MaximizedCall*)user;
    call.maximized      = (SDL_GetWindowFlags((SDL_Window*)call.window) & SDL_WINDOW_HIDDEN) == 0;
}

b8 os_window_shown (OS_Window* window)
{
    MaximizedCall call = { .window = window, .maximized = false };
    if (window)
    {
        os_run_on_window_thread(window_shown_now, &call);
    }
    return call.maximized;
}

#if HAVE_WINDOW_THREAD
// Moves what SDL holds at this moment into the queue, so that a poll sees every event pushed
// before it. It runs inside the window thread's pump, which must not pump again.
static void queue_pending_events (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    (void)user;
    OS_QueuedEvent queued = {};
    while (SDL_PeepEvents(&queued.event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1)
    {
        queue_event(queued);
    }
}
#endif

// The next event for os_poll_events: from the window thread's queue where there is one
static b8 next_event (OS_QueuedEvent& out)
{
#if HAVE_WINDOW_THREAD
    if (os_state.queue)
    {
        SDL_LockMutex(os_state.queue_mutex);
        const b8 any = os_state.queue_count > 0;
        if (any)
        {
            out                  = os_state.queue[os_state.queue_head];
            os_state.queue_head  = (os_state.queue_head + 1) % OS_WINDOW_EVENT_QUEUE;
            os_state.queue_count--;
        }
        SDL_UnlockMutex(os_state.queue_mutex);
        if (any && (out.event.type == SDL_EVENT_TEXT_INPUT || out.event.type == SDL_EVENT_TEXT_EDITING))
        {
            out.event.text.text = out.text;
        }
        return any;
    }
#endif
#if HAVE_MACOS
    // SDL frees the text of text and drop events at the next pump of the thread that took them,
    // so those and everything after them stay for the main thread
    if (!on_main_thread() && SDL_PeepEvents(&out.event, 1, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1)
    {
        const u32 type = out.event.type;
        if (type == SDL_EVENT_TEXT_INPUT || type == SDL_EVENT_TEXT_EDITING || type == SDL_EVENT_TEXT_EDITING_CANDIDATES || type == SDL_EVENT_DROP_FILE || type == SDL_EVENT_DROP_TEXT)
        {
            return false;
        }
    }
#endif
    return SDL_PeepEvents(&out.event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1;
}

b8 os_has_audio_device_ids ()
{
    return HAVE_WINDOWS != 0;
}

b8 os_default_audio_device (b8 input, wchar_t* out_id, usize count)
{
    if (!out_id || count == 0)
    {
        return false;
    }
    out_id[0] = 0;
#if HAVE_WINDOWS
    // The app may own COM on this thread in either model; a mismatch still lets us use it
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    LPWSTR               id         = nullptr;
    b8                   found      = false;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(input ? eCapture : eRender, eConsole, &device)) && SUCCEEDED(device->GetId(&id)))
    {
        usize i = 0;
        for (; id[i] && i + 1 < count; i++)
        {
            out_id[i] = id[i];
        }
        out_id[i] = 0;
        found     = id[i] == 0;
        if (!found)
        {
            out_id[0] = 0;
        }
    }
    if (id)
    {
        CoTaskMemFree(id);
    }
    if (device)
    {
        device->Release();
    }
    if (enumerator)
    {
        enumerator->Release();
    }
    if (SUCCEEDED(initialized))
    {
        CoUninitialize();
    }
    return found;
#else
    (void)input;
    return false;
#endif
}

// A synthetic event goes straight into the queue for the next os_poll_queued_events: SDL's queue
// reaches it only once the window thread wakes, frames later
static b8 push_event (SDL_Event& event)
{
#if HAVE_WINDOW_THREAD
    if (os_state.window_thread)
    {
        OS_QueuedEvent queued = {};
        queued.event          = event;
        queue_event(queued);
        return true;
    }
#endif
    return SDL_PushEvent(&event);
}

b8 os_push_mouse_event (OS_Window* window, f32 x, f32 y, OS_Key button, b8 down)
{
    if (!window)
    {
        return false;
    }
    const SDL_WindowID window_id = SDL_GetWindowID((SDL_Window*)window);
    os_state.pushed_window = window_id;
    if (button == OS_Key::None)
    {
        SDL_Event motion        = {};
        motion.type             = SDL_EVENT_MOUSE_MOTION;
        motion.motion.timestamp = SDL_GetTicksNS();
        motion.motion.windowID  = window_id;
        motion.motion.x         = x;
        motion.motion.y         = y;
        motion.motion.xrel      = x - os_state.pushed_x;
        motion.motion.yrel      = y - os_state.pushed_y;
        os_state.pushed_x       = x;
        os_state.pushed_y       = y;
        return push_event(motion);
    }
    u8 sdl_button = 0;
    switch (button)
    {
    case OS_Key::MouseLeft:
        sdl_button = SDL_BUTTON_LEFT;
        break;
    case OS_Key::MouseRight:
        sdl_button = SDL_BUTTON_RIGHT;
        break;
    case OS_Key::MouseMiddle:
        sdl_button = SDL_BUTTON_MIDDLE;
        break;
    default:
        return false;
    }
    SDL_Event press        = {};
    press.type             = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    press.button.timestamp = SDL_GetTicksNS();
    press.button.windowID  = window_id;
    press.button.button    = sdl_button;
    press.button.down      = down;
    press.button.clicks    = 1;
    press.button.x         = x;
    press.button.y         = y;
    return push_event(press);
}

b8 os_push_wheel_event (OS_Window* window, f32 dy)
{
    if (!window)
    {
        return false;
    }
    SDL_Event wheel       = {};
    wheel.type            = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.timestamp = SDL_GetTicksNS();
    wheel.wheel.windowID  = SDL_GetWindowID((SDL_Window*)window);
    wheel.wheel.y         = dy;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    return push_event(wheel);
}

b8 os_push_focus_lost (OS_Window* window)
{
    if (!window)
    {
        return false;
    }
    SDL_Event focus        = {};
    focus.type             = SDL_EVENT_WINDOW_FOCUS_LOST;
    focus.window.timestamp = SDL_GetTicksNS();
    focus.window.windowID  = SDL_GetWindowID((SDL_Window*)window);
    return push_event(focus);
}

b8 os_push_key_event (OS_Window* window, OS_Key key, b8 down, b8 repeat)
{
    if (!window || key == OS_Key::None)
    {
        return false;
    }
    const SDL_Scancode scancode = scancode_from_key(key);
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        return false;
    }
    SDL_Event event     = {};
    event.type          = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = SDL_GetTicksNS();
    event.key.windowID  = SDL_GetWindowID((SDL_Window*)window);
    event.key.scancode  = scancode;
    event.key.key       = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    event.key.down      = down;
    event.key.repeat    = repeat;
    return push_event(event);
}

void os_pump_window_events ()
{
    if (!os_state.initialized)
    {
        return;
    }
#if HAVE_WINDOW_THREAD
    if (os_state.window_thread)
    {
        os_run_on_window_thread(queue_pending_events, nullptr);
    }
#else
    if (on_main_thread())
    {
        serve_main_calls();
        if (!app_runs_event_loop())
        {
            SDL_PumpEvents();
        }
    }
#endif
}

u32 os_poll_events (OS_Event* out_events, u32 capacity)
{
    os_pump_window_events();
    return os_poll_queued_events(out_events, capacity);
}

u32 os_poll_queued_events (OS_Event* out_events, u32 capacity)
{
    ASSERT(out_events || capacity == 0);
    // Events beyond `capacity` wait for the next poll: after a stall, discarding them lost key
    // releases and kept the keys held
    u32            count  = 0;
    OS_QueuedEvent queued = {};
    while (count < capacity && next_event(queued))
    {
        const SDL_Event& event = queued.event;
        if (os_state.raw_hook)
        {
            // Windows may send a window clicks without the moves before them (in Unity's editor,
            // whose input takes the mouse moves), and ImGui clicks where the last move was
            const b8 button = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
            if (button && (event.button.windowID != os_state.hook_motion_window || event.button.x != os_state.hook_motion_x || event.button.y != os_state.hook_motion_y))
            {
                SDL_Event motion        = {};
                motion.type             = SDL_EVENT_MOUSE_MOTION;
                motion.motion.timestamp = event.button.timestamp;
                motion.motion.windowID  = event.button.windowID;
                motion.motion.which     = event.button.which;
                motion.motion.x         = event.button.x;
                motion.motion.y         = event.button.y;
                os_state.raw_hook(&motion, true, os_state.raw_hook_user);
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION || button)
            {
                os_state.hook_motion_window = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.windowID : event.button.windowID;
                os_state.hook_motion_x      = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
                os_state.hook_motion_y      = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
            }
            const b8 input = button || event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL || event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_TEXT_EDITING;
            os_state.raw_hook(&event, input, os_state.raw_hook_user);
        }

        OS_Event translated = {};
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (event.key.repeat)
            {
                continue;
            }
            translated.type = event.type == SDL_EVENT_KEY_DOWN ? OS_EventType::KeyDown : OS_EventType::KeyUp;
            translated.key  = key_from_scancode(event.key.scancode);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            translated.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? OS_EventType::KeyDown : OS_EventType::KeyUp;
            translated.key  = key_from_mouse_button(event.button.button);
            translated.x    = event.button.x;
            translated.y    = event.button.y;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            translated.type = OS_EventType::MouseMotion;
            translated.dx   = event.motion.xrel;
            translated.dy   = event.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            translated.type = OS_EventType::MouseWheel;
            translated.dy   = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            translated.type = OS_EventType::FocusLost;
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            translated.type = OS_EventType::WindowClose;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            translated.type   = OS_EventType::WindowResize;
            translated.width  = event.window.data1 > 0 ? (u32)event.window.data1 : 0;
            translated.height = event.window.data2 > 0 ? (u32)event.window.data2 : 0;
            break;
        default:
            continue;
        }

        if (translated.type == OS_EventType::None || ((translated.type == OS_EventType::KeyDown || translated.type == OS_EventType::KeyUp) && translated.key == OS_Key::None))
        {
            continue;
        }
        if (count < capacity)
        {
            out_events[count++] = translated;
        }
    }
    return count;
}

void os_set_raw_event_hook (OS_RawEventHook hook, void* user)
{
    os_state.raw_hook      = hook;
    os_state.raw_hook_user = user;
}

//
// Streaming files
//

#if HAVE_WINDOWS

// Native rather than SDL's, which shares nothing with readers while it writes
struct OS_File
{
    HANDLE handle;
    b8     failed;
};

OS_File* os_file_create (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    if (!path || !path[0] || !to_wide(path, wide, ARRAY_SIZE(wide)))
    {
        return nullptr;
    }
    const HANDLE handle = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("Cannot create %s: error %lu", path, GetLastError());
        return nullptr;
    }
    OS_File* file = (OS_File*)os_memory_alloc(sizeof(OS_File));
    file->handle  = handle;
    return file;
}

// Shared for reading and appending: a test driver reads a log as it grows, and two processes may
// write one; SDL opens files for writing to this process alone
OS_File* os_file_append (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    if (!path || !path[0] || !to_wide(path, wide, ARRAY_SIZE(wide)))
    {
        return nullptr;
    }
    const HANDLE handle = CreateFileW(wide, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return nullptr;
    }
    OS_File* file = (OS_File*)os_memory_alloc(sizeof(OS_File));
    file->handle  = handle;
    file->failed  = false;
    return file;
}

b8 os_file_write (OS_File* file, const void* data, usize size)
{
    if (!file || (!data && size > 0))
    {
        return false;
    }
    usize done = 0;
    while (done < size && !file->failed)
    {
        const usize rest    = size - done;
        const DWORD part    = rest > 0x40000000 ? 0x40000000 : (DWORD)rest;
        DWORD       written = 0;
        file->failed        = !WriteFile(file->handle, (const u8*)data + done, part, &written, nullptr) || written != part;
        done += written;
    }
    return !file->failed;
}

// Writes are unbuffered, so readers see them already; nothing to push out
b8 os_file_flush (OS_File* file) // NOLINT(misc-const-correctness): the signature of every platform
{
    return file && !file->failed;
}

b8 os_file_close (OS_File* file)
{
    if (!file)
    {
        return false;
    }
    const b8 closed = CloseHandle(file->handle) && !file->failed;
    os_memory_release(file);
    return closed;
}

#else

OS_File* os_file_create (const char* path)
{
    if (!path || !path[0])
    {
        return nullptr;
    }
    SDL_IOStream* stream = SDL_IOFromFile(path, "wb");
    if (!stream)
    {
        LOG_ERROR("Cannot create %s: %s", path, SDL_GetError());
        return nullptr;
    }
    return (OS_File*)stream;
}

OS_File* os_file_append (const char* path)
{
    if (!path || !path[0])
    {
        return nullptr;
    }
    return (OS_File*)SDL_IOFromFile(path, "ab");
}

b8 os_file_write (OS_File* file, const void* data, usize size)
{
    if (!file || (!data && size > 0))
    {
        return false;
    }
    return SDL_WriteIO((SDL_IOStream*)file, data, size) == size;
}

b8 os_file_flush (OS_File* file)
{
    return file && SDL_FlushIO((SDL_IOStream*)file);
}

b8 os_file_close (OS_File* file)
{
    if (!file)
    {
        return false;
    }
    return SDL_CloseIO((SDL_IOStream*)file);
}

#endif

//
// Mutex
//

OS_Mutex* os_mutex_create ()
{
    return (OS_Mutex*)SDL_CreateMutex();
}

void os_mutex_destroy (OS_Mutex* mutex)
{
    SDL_DestroyMutex((SDL_Mutex*)mutex);
}

// A mutex is held for short stretches, so a waiter spins a little first. On macOS the waiting
// main thread serves window calls, as the holder may wait for one, and sleeps on the queue's
// semaphore, which every unlock and queued call signals
void os_mutex_lock (OS_Mutex* mutex)
{
    SDL_Mutex* const sdl = (SDL_Mutex*)mutex;
    const u64        start = os_monotonic_ns();
    do
    {
        if (SDL_TryLockMutex(sdl))
        {
            return;
        }
    } while (os_monotonic_ns() - start < OS_MUTEX_SPIN_NS);
#if HAVE_MACOS
    if (os_main_queue.mutex && on_main_thread())
    {
        SDL_AddAtomicInt(&os_main_queue.main_waiting, 1);
        for (;;)
        {
            serve_main_calls();
            if (SDL_TryLockMutex(sdl))
            {
                break;
            }
            SDL_WaitSemaphoreTimeout(os_main_queue.wake, 5);
        }
        SDL_AddAtomicInt(&os_main_queue.main_waiting, -1);
        return;
    }
#endif
#if HAVE_WINDOW_THREAD
    // The holder may wait for the window thread: a frame's window tick waits for the runtime
    // lock, which xrDestroySession holds while the panels shut down there
    if (on_window_thread())
    {
        SDL_AddAtomicInt(&os_state.window_waiting, 1);
        for (;;)
        {
            run_window_thread_call(0);
            if (SDL_TryLockMutex(sdl))
            {
                break;
            }
            SDL_WaitSemaphoreTimeout(os_state.window_wake, 5);
        }
        SDL_AddAtomicInt(&os_state.window_waiting, -1);
        return;
    }
#endif
    SDL_LockMutex(sdl);
}

void os_mutex_unlock (OS_Mutex* mutex)
{
    SDL_UnlockMutex((SDL_Mutex*)mutex);
#if HAVE_MACOS
    if (SDL_GetAtomicInt(&os_main_queue.main_waiting) > 0)
    {
        SDL_SignalSemaphore(os_main_queue.wake);
    }
#endif
#if HAVE_WINDOW_THREAD
    if (SDL_GetAtomicInt(&os_state.window_waiting) > 0)
    {
        SDL_SignalSemaphore(os_state.window_wake);
    }
#endif
}

//
// Semaphores and threads
//

OS_Semaphore* os_semaphore_create (u32 initial)
{
    return (OS_Semaphore*)SDL_CreateSemaphore(initial);
}

void os_semaphore_destroy (OS_Semaphore* semaphore)
{
    SDL_DestroySemaphore((SDL_Semaphore*)semaphore);
}

void os_semaphore_wait (OS_Semaphore* semaphore)
{
    SDL_WaitSemaphore((SDL_Semaphore*)semaphore);
}

b8 os_semaphore_try_wait (OS_Semaphore* semaphore)
{
    return SDL_TryWaitSemaphore((SDL_Semaphore*)semaphore);
}

void os_semaphore_signal (OS_Semaphore* semaphore)
{
    SDL_SignalSemaphore((SDL_Semaphore*)semaphore);
}

OS_Thread* os_thread_create (const char* name, OS_ThreadFunction* function, void* user)
{
    return (OS_Thread*)SDL_CreateThread((SDL_ThreadFunction)function, name, user);
}

i32 os_thread_join (OS_Thread* thread)
{
    int result = 0;
    SDL_WaitThread((SDL_Thread*)thread, &result);
    return result;
}

u64 os_thread_current_id ()
{
    return (u64)SDL_GetCurrentThreadID();
}

//
// Zone profiler
//

namespace
{

struct ProfileEvent
{
    const char* name;
    u64         start_ns;
    u64         end_ns;
};

struct ProfileOpen
{
    const char* name;
    u64         start_ns;
    i32         generation;
};

// One per thread that ever recorded, kept for later captures: the thread may still write while a
// capture stops. Only its thread writes; `count` publishes the events
struct ProfileRing
{
    ProfileEvent  events[OS_PROFILE_RING_EVENTS];
    SDL_AtomicInt count;
    u32           capture_start;
    u64           thread_id;
    char          thread_name[64];
    ProfileOpen   open[OS_PROFILE_MAX_DEPTH];
    u32           depth;
};

struct ProfileState
{
    SDL_AtomicInt active;
    SDL_AtomicInt generation;
    SDL_SpinLock  lock;
    ProfileRing*  rings[OS_PROFILE_MAX_THREADS];
    u32           rings_count;
    OS_File*      file;
    u64           start_ns;
};

struct ProfileTotal
{
    const char* name;
    u32         calls;
    u64         total_ns;
    u64         max_ns;
};

} // namespace

static ProfileState              os_profile = {};
static thread_local ProfileRing* os_profile_ring = nullptr;

static void profile_thread_name (ProfileRing& ring)
{
    ring.thread_name[0] = 0;
#if HAVE_WINDOWS
    typedef HRESULT (WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);
    const GetThreadDescriptionFn get = (GetThreadDescriptionFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    PWSTR description = nullptr;
    if (get && SUCCEEDED(get(GetCurrentThread(), &description)) && description)
    {
        from_wide(description, ring.thread_name, sizeof(ring.thread_name));
        LocalFree(description);
    }
#else
    pthread_getname_np(pthread_self(), ring.thread_name, sizeof(ring.thread_name));
#endif
    // Linux names the main thread after the process; macOS leaves it empty
    if (!ring.thread_name[0])
    {
        if (HAVE_MACOS && os_is_window_thread())
        {
            cstr_copy(ring.thread_name, sizeof(ring.thread_name), "main");
        }
        else
        {
            cstr_format(ring.thread_name, sizeof(ring.thread_name), "thread %llu", (unsigned long long)ring.thread_id);
        }
    }
}

static ProfileRing* profile_ring ()
{
    if (os_profile_ring)
    {
        return os_profile_ring;
    }
    SDL_LockSpinlock(&os_profile.lock);
    if (os_profile.rings_count < OS_PROFILE_MAX_THREADS)
    {
        ProfileRing* ring = (ProfileRing*)os_memory_alloc(sizeof(ProfileRing));
        ring->thread_id   = os_thread_current_id();
        profile_thread_name(*ring);
        ring->capture_start = 0;
        os_profile.rings[os_profile.rings_count++] = ring;
        os_profile_ring = ring;
    }
    SDL_UnlockSpinlock(&os_profile.lock);
    return os_profile_ring;
}

b8 os_profile_active ()
{
    return SDL_GetAtomicInt(&os_profile.active) != 0;
}

void os_profile_begin (const char* name)
{
    ProfileRing* ring = profile_ring();
    if (!ring)
    {
        return;
    }
    const u32 depth = ring->depth++;
    if (depth < OS_PROFILE_MAX_DEPTH)
    {
        ring->open[depth] = { .name = name, .start_ns = os_monotonic_ns(), .generation = SDL_GetAtomicInt(&os_profile.generation) };
    }
}

void os_profile_end ()
{
    ProfileRing* ring = os_profile_ring;
    if (!ring || ring->depth == 0)
    {
        return;
    }
    const u32 depth = --ring->depth;
    if (depth >= OS_PROFILE_MAX_DEPTH)
    {
        return;
    }
    const ProfileOpen& open = ring->open[depth];
    if (!os_profile_active() || open.generation != SDL_GetAtomicInt(&os_profile.generation))
    {
        return;
    }
    const u32 count = (u32)SDL_GetAtomicInt(&ring->count);
    ring->events[count % OS_PROFILE_RING_EVENTS] = { .name = open.name, .start_ns = open.start_ns, .end_ns = os_monotonic_ns() };
    SDL_SetAtomicInt(&ring->count, (i32)(count + 1));
}

b8 os_profile_start (const char* path)
{
    if (!path || !path[0] || os_profile_active())
    {
        return false;
    }
    OS_File* file = os_file_create(path);
    if (!file)
    {
        LOG_ERROR("Cannot write the profile %s", path);
        return false;
    }
    SDL_LockSpinlock(&os_profile.lock);
    os_profile.file     = file;
    os_profile.start_ns = os_monotonic_ns();
    for (u32 i = 0; i < os_profile.rings_count; i++)
    {
        os_profile.rings[i]->capture_start = (u32)SDL_GetAtomicInt(&os_profile.rings[i]->count);
    }
    SDL_AddAtomicInt(&os_profile.generation, 1);
    SDL_SetAtomicInt(&os_profile.active, 1);
    SDL_UnlockSpinlock(&os_profile.lock);
    LOG_INFO("Profiling into %s", path);
    return true;
}

static void profile_write (char* buffer, usize& used, usize capacity, OS_File* file, const char* text, usize length)
{
    if (used + length > capacity)
    {
        os_file_write(file, buffer, used);
        used = 0;
    }
    memory_copy(buffer + used, text, length);
    used += length;
}

static i32 compare_totals (const ProfileTotal* a, const ProfileTotal* b)
{
    return a->total_ns > b->total_ns ? -1 : (a->total_ns < b->total_ns ? 1 : 0);
}

void os_profile_stop ()
{
    if (!os_profile_active())
    {
        return;
    }
    SDL_LockSpinlock(&os_profile.lock);
    SDL_SetAtomicInt(&os_profile.active, 0);
    ProfileRing*       rings[OS_PROFILE_MAX_THREADS] = {}; // NOLINT(misc-const-correctness): SDL_GetAtomicInt takes a mutable atomic
    const u32          rings_count                   = os_profile.rings_count;
    for (u32 i = 0; i < rings_count; i++)
    {
        rings[i] = os_profile.rings[i];
    }
    OS_File* file       = os_profile.file;
    os_profile.file     = nullptr;
    const u64 start_ns  = os_profile.start_ns;
    SDL_UnlockSpinlock(&os_profile.lock);

    const usize   capacity = 1 << 16;
    char*         buffer   = (char*)os_memory_alloc(capacity);
    usize         used     = 0;
    ProfileTotal* totals   = (ProfileTotal*)os_memory_alloc(sizeof(ProfileTotal) * 1024);
    u32           totals_count = 0;
    b8            first    = true;
    char          line[512];
    profile_write(buffer, used, capacity, file, "{\"traceEvents\":[", 16);
    for (u32 r = 0; r < rings_count; r++)
    {
        ProfileRing&       ring  = *rings[r];
        const u32          end   = (u32)SDL_GetAtomicInt(&ring.count);
        const u32          begin = end - ring.capture_start > OS_PROFILE_RING_EVENTS ? end - OS_PROFILE_RING_EVENTS : ring.capture_start;
        if (begin == end)
        {
            continue;
        }
        usize length = cstr_format(line, sizeof(line), "%s{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%llu,\"args\":{\"name\":\"%s\"}}", first ? "" : ",\n", (unsigned long long)ring.thread_id, ring.thread_name);
        profile_write(buffer, used, capacity, file, line, zm_min_usize(length, sizeof(line) - 1));
        first = false;
        for (u32 i = begin; i != end; i++)
        {
            const ProfileEvent& event    = ring.events[i % OS_PROFILE_RING_EVENTS];
            const u64           duration = event.end_ns - event.start_ns;
            length = cstr_format(line, sizeof(line), ",\n{\"name\":\"%s\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,\"pid\":1,\"tid\":%llu}", event.name, (f64)(event.start_ns - start_ns) * 1e-3, (f64)duration * 1e-3, (unsigned long long)ring.thread_id);
            profile_write(buffer, used, capacity, file, line, zm_min_usize(length, sizeof(line) - 1));
            u32 t = 0;
            while (t < totals_count && totals[t].name != event.name && !cstr_equal(totals[t].name, event.name))
            {
                t++;
            }
            if (t == totals_count && totals_count < 1024)
            {
                totals[totals_count++] = { .name = event.name };
            }
            if (t < totals_count)
            {
                totals[t].calls++;
                totals[t].total_ns += duration;
                totals[t].max_ns    = totals[t].max_ns > duration ? totals[t].max_ns : duration;
            }
        }
    }
    profile_write(buffer, used, capacity, file, "\n]}\n", 4);
    os_file_write(file, buffer, used);
    os_file_close(file);

    quicksort(totals, totals_count, compare_totals);
    LOG_INFO("Profile: %u zones, the ones of most total time:", totals_count);
    for (u32 t = 0; t < totals_count && t < 20; t++)
    {
        const ProfileTotal& total = totals[t];
        LOG_INFO("  %-28s %8u calls %10.2f ms total %8.3f ms mean %8.3f ms max", total.name, total.calls, (f64)total.total_ns * 1e-6, (f64)total.total_ns * 1e-6 / (f64)total.calls, (f64)total.max_ns * 1e-6);
    }
    os_memory_release(totals);
    os_memory_release(buffer);
}

//
// Logging and asserts
//

void os_set_log_callback (OS_LogCallback* callback, void* user)
{
    if (!os_state.log_mutex)
    {
        os_state.log_mutex = SDL_CreateMutex();
    }
    SDL_LockMutex(os_state.log_mutex);
    os_state.log_callback      = callback;
    os_state.log_callback_user = user;
    SDL_UnlockMutex(os_state.log_mutex);
}

void os_log (OS_LogLevel level, const char* fmt, ...)
{
    va_list args = {};
    va_start(args, fmt);
    os_log_v(level, fmt, args);
    va_end(args);
}

void os_log_text (OS_LogLevel level, const char* text)
{
    if (level < os_state.log_level)
    {
        return;
    }

    SDL_LogPriority priority = SDL_LOG_PRIORITY_INFO;
    switch (level)
    {
    case OS_LogLevel::Debug:
        priority = SDL_LOG_PRIORITY_DEBUG;
        break;
    case OS_LogLevel::Info:
        priority = SDL_LOG_PRIORITY_INFO;
        break;
    case OS_LogLevel::Warning:
        priority = SDL_LOG_PRIORITY_WARN;
        break;
    case OS_LogLevel::Error:
        priority = SDL_LOG_PRIORITY_ERROR;
        break;
    }

    log_append(level, OS_LogSource::Sim, text);

    // While capturing, stderr is our own pipe; write straight to the terminal instead
    if (os_state.capture_active)
    {
        fd_write(os_state.saved_stderr, "puppetxr: ", 7);
        fd_write(os_state.saved_stderr, text, cstr_length(text));
        fd_write(os_state.saved_stderr, "\n", 1);
        return;
    }
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s", text);
}

void os_log_v (OS_LogLevel level, const char* fmt, va_list args)
{
    if (level < os_state.log_level)
    {
        return;
    }
    char text[OS_LOG_LINE_LENGTH];
    cstr_format_v(text, sizeof(text), fmt, args);
    os_log_text(level, text);
}

b8 os_capture_output_begin ()
{
    if (os_state.capture_active)
    {
        return true;
    }
    i32 fds[2];
#if HAVE_WINDOWS
    if (_pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0)
#else
    if (pipe(fds) != 0)
#endif
    {
        return false;
    }
#if !HAVE_WINDOWS
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
#endif
    (void)fflush(stdout);
    (void)fflush(stderr);
    os_state.saved_stdout = fd_dup(1);
    os_state.saved_stderr = fd_dup(2);
    fd_dup2(fds[1], 1);
    fd_dup2(fds[1], 2);
    fd_close(fds[1]);
    os_state.capture_read   = fds[0];
    os_state.capture_active = true;
    // stdout becomes fully buffered on a pipe; line buffering keeps the app's prints timely. The
    // Windows C runtime has no line buffering, so stdout goes unbuffered there.
    (void)setvbuf(stdout, nullptr, HAVE_WINDOWS ? _IONBF : _IOLBF, 0);
    return true;
}

void os_capture_output_poll ()
{
    if (!os_state.capture_active)
    {
        return;
    }
    char buffer[4096];
    while (true)
    {
        const i64 count = fd_read_available(os_state.capture_read, buffer, sizeof(buffer));
        if (count <= 0)
        {
            break;
        }
        fd_write(os_state.saved_stdout, buffer, (usize)count);
        for (i64 i = 0; i < count; i++)
        {
            const char c = buffer[i];
            if (c == '\n' || os_state.capture_partial_length == OS_LOG_LINE_LENGTH - 1)
            {
                os_state.capture_partial[os_state.capture_partial_length] = 0;
                if (os_state.capture_partial_length > 0)
                {
                    log_append(OS_LogLevel::Info, OS_LogSource::App, os_state.capture_partial);
                }
                os_state.capture_partial_length = 0;
                if (c == '\n')
                {
                    continue;
                }
            }
            if (c != '\r')
            {
                os_state.capture_partial[os_state.capture_partial_length++] = c;
            }
        }
    }
}

void os_capture_output_end ()
{
    if (!os_state.capture_active)
    {
        return;
    }
    os_capture_output_poll();
    (void)fflush(stdout);
    (void)fflush(stderr);
    if (os_state.saved_stdout >= 0)
    {
        fd_dup2(os_state.saved_stdout, 1);
        fd_close(os_state.saved_stdout);
    }
    if (os_state.saved_stderr >= 0)
    {
        fd_dup2(os_state.saved_stderr, 2);
        fd_close(os_state.saved_stderr);
    }
    fd_close(os_state.capture_read);
    os_state.capture_active = false;
}

u32 os_log_history_count ()
{
    return os_state.log_count;
}

const OS_LogLine* os_log_history (u32 index)
{
    if (index >= os_state.log_count)
    {
        return nullptr;
    }
    return &os_state.log_lines[(os_state.log_head + index) % OS_LOG_HISTORY];
}

u64 os_log_sequence ()
{
    return os_state.log_sequence;
}

b8 os_claim_once (u32* flag) // NOLINT(readability-non-const-parameter,misc-const-correctness): written by the atomic exchange
{
#if defined(_MSC_VER) && !defined(__clang__)
    static_assert(sizeof(long) == sizeof(u32));
    return _InterlockedExchange(reinterpret_cast<volatile long*>(flag), 1) == 0;
#else
    return __atomic_exchange_n(flag, 1u, __ATOMIC_ACQ_REL) == 0;
#endif
}

void os_set_log_level (OS_LogLevel level)
{
    os_state.log_level = level;
}

OS_AssertMode os_default_assert_mode ()
{
    return PUPPETXR_DIST ? OS_AssertMode::Log : OS_AssertMode::Break;
}

void os_set_assert_mode (OS_AssertMode mode)
{
    os_state.assert_mode = mode;
}

u32 os_assert_count ()
{
    return os_state.assert_count;
}

b8 os_report_assert (const char* expression, const char* function, const char* file, i32 line, const char* message, ...)
{
    os_state.assert_count++;

    char prefix[512] = {};
    cstr_format(prefix, sizeof(prefix), "Assertion failed: %s (%s, %s:%d) ", expression, function, file, line);

    if (message)
    {
        va_list args = {};
        va_start(args, message);
        log_prefixed(OS_LogLevel::Error, prefix, message, args);
        va_end(args);
    }
    else
    {
        os_log(OS_LogLevel::Error, "%s", prefix);
    }

    return os_state.assert_mode == OS_AssertMode::Break;
}

b8 os_report_failure (const char* function, const char* file, i32 line, const char* message, ...)
{
    os_state.assert_count++;

    char prefix[512] = {};
    cstr_format(prefix, sizeof(prefix), "Failure in %s (%s:%d): ", function, file, line);

    va_list args = {};
    va_start(args, message);
    log_prefixed(OS_LogLevel::Error, prefix, message, args);
    va_end(args);

    return os_state.assert_mode == OS_AssertMode::Break;
}

// Breaking without a debugger kills the host app with SIGTRAP, so only break when one is attached
static b8 debugger_attached ()
{
#if HAVE_LINUX
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file("/proc/self/status", &data, &size))
    {
        return false;
    }
    const char* marker   = "TracerPid:";
    b8          attached = false;
    for (usize i = 0; i + 10 < size; i++)
    {
        if (memory_equal(data + i, marker, 10))
        {
            usize k = i + 10;
            while (k < size && (data[k] == ' ' || data[k] == '\t'))
            {
                k++;
            }
            attached = k < size && data[k] != '0';
            break;
        }
    }
    os_memory_release(data);
    return attached;
#elif HAVE_WINDOWS
    return IsDebuggerPresent() != 0;
#else
    return false;
#endif
}

void os_trigger_breakpoint ()
{
    if (debugger_attached())
    {
        SDL_TriggerBreakpoint();
    }
}

//
// Keys
//

const char* os_key_name (OS_Key key)
{
    return (u32)key < (u32)OS_Key::Count ? os_key_names[(u32)key] : "none";
}

// Keys are physical positions named after US QWERTY; the layout names them, as the key left of
// 1 that is ^ on German layouts
void os_key_label (OS_Key key, char* out, usize size)
{
    const SDL_Scancode scancode = scancode_from_key(key);
    const char*        label    = scancode != SDL_SCANCODE_UNKNOWN ? SDL_GetKeyName(SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false)) : nullptr;
    cstr_copy(out, size, label && label[0] ? label : os_key_name(key));
}

b8 os_key_from_name (const char* name, OS_Key& out_key)
{
    for (u32 i = 0; i < (u32)OS_Key::Count; i++)
    {
        if (cstr_equal(name, os_key_names[i]))
        {
            out_key = (OS_Key)i;
            return true;
        }
    }
    out_key = OS_Key::None;
    return false;
}

//
// Memory and string helpers
//

// The C library leaves null pointers undefined even for size 0, which callers pass for empty ranges
void* memory_move (void* dest, const void* source, usize size)
{
    if (size == 0)
    {
        return dest;
    }
    return SDL_memmove(dest, source, size);
}

void* memory_copy (void* dest, const void* source, usize size)
{
    if (size == 0)
    {
        return dest;
    }
    return SDL_memcpy(dest, source, size);
}

void* memory_zero (void* dest, usize size)
{
    if (size == 0)
    {
        return dest;
    }
    return SDL_memset(dest, 0, size);
}

b8 memory_equal (const void* a, const void* b, usize size)
{
    if (size == 0)
    {
        return true;
    }
    return SDL_memcmp(a, b, size) == 0;
}

b8 cstr_equal (const char* a, const char* b)
{
    if (!a || !b)
    {
        return a == b;
    }
    return SDL_strcmp(a, b) == 0;
}

usize cstr_length (const char* str)
{
    return str ? SDL_strlen(str) : 0;
}

b8 cstr_copy (char* dest, usize dest_size, const char* source)
{
    ASSERT(dest && dest_size > 0);
    if (!source)
    {
        dest[0] = 0;
        return true;
    }
    // Callers pass a field as its own default; strlcpy's overlap is undefined and traps on macOS
    if (dest == source)
    {
        return SDL_strlen(dest) < dest_size;
    }
    return SDL_strlcpy(dest, source, dest_size) < dest_size;
}

b8 cstr_starts_with (const char* str, const char* prefix)
{
    if (!str || !prefix)
    {
        return false;
    }
    return SDL_strncmp(str, prefix, SDL_strlen(prefix)) == 0;
}

const char* cstr_find (const char* str, const char* needle)
{
    if (!str || !needle)
    {
        return nullptr;
    }
    return SDL_strstr(str, needle);
}

usize cstr_format (char* dest, usize dest_size, const char* fmt, ...)
{
    va_list args = {};
    va_start(args, fmt);
    const usize length = cstr_format_v(dest, dest_size, fmt, args);
    va_end(args);
    return length;
}

usize cstr_format_v (char* dest, usize dest_size, const char* fmt, va_list args)
{
    const i32 length = SDL_vsnprintf(dest, dest_size, fmt, args);
    return length < 0 ? 0 : (usize)length;
}

void quicksort_raw (void* data, usize count, usize element_size, i32 (*compare)(const void*, const void*))
{
    SDL_qsort(data, count, element_size, compare);
}

//
// Files, processes, sockets and lock files
//

OS_Process* os_process_spawn (const char* const* argv, const char* stdin_path, const char* stdout_path)
{
    if (!argv || !argv[0])
    {
        return nullptr;
    }
    SDL_IOStream* input  = stdin_path ? SDL_IOFromFile(stdin_path, "rb") : nullptr;
    SDL_IOStream* output = stdout_path ? SDL_IOFromFile(stdout_path, "wb") : nullptr;
    SDL_Process*  process = nullptr;
    if ((stdin_path && !input) || (stdout_path && !output))
    {
        LOG_ERROR("Cannot open the streams of %s: %s", argv[0], SDL_GetError());
    }
    else
    {
        SDL_Environment*       environment = SDL_CreateEnvironment(true);
        const SDL_PropertiesID properties  = SDL_CreateProperties();
        // Properties hold non-const pointers; SDL only reads the arguments
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)(uintptr_t)argv);
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, input ? SDL_PROCESS_STDIO_REDIRECT : SDL_PROCESS_STDIO_INHERITED);
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_POINTER, input);
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, output ? SDL_PROCESS_STDIO_REDIRECT : SDL_PROCESS_STDIO_INHERITED);
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, output);
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, output != nullptr);
        process = SDL_CreateProcessWithProperties(properties);
        if (!process)
        {
            LOG_ERROR("Cannot start %s: %s", argv[0], SDL_GetError());
        }
        SDL_DestroyProperties(properties);
        SDL_DestroyEnvironment(environment);
    }
    // The child has its own copies of the streams by now
    if (input)
    {
        SDL_CloseIO(input);
    }
    if (output)
    {
        SDL_CloseIO(output);
    }
    return (OS_Process*)process;
}

b8 os_process_wait (OS_Process* process, u32 timeout_ms, i32& out_exit_code)
{
    out_exit_code = -1;
    if (!process)
    {
        return false;
    }
    const u64 deadline = os_monotonic_ns() + (u64)timeout_ms * 1000000ull;
    while (true)
    {
        int exit_code = -1;
        if (SDL_WaitProcess((SDL_Process*)process, false, &exit_code))
        {
            out_exit_code = exit_code;
            return true;
        }
        if (os_monotonic_ns() >= deadline)
        {
            return false;
        }
        os_sleep_ns(1000000);
    }
}

void os_process_release (OS_Process* process)
{
    if (process)
    {
        int exit_code = 0;
        if (!SDL_WaitProcess((SDL_Process*)process, false, &exit_code))
        {
            SDL_KillProcess((SDL_Process*)process, true);
            SDL_WaitProcess((SDL_Process*)process, true, &exit_code);
        }
        SDL_DestroyProcess((SDL_Process*)process);
    }
}

b8 os_create_temp_directory (const char* prefix, char* out_path, usize size)
{
    char base[OS_MAX_PATH];
    if (!os_temp_dir(base, sizeof(base)))
    {
        return false;
    }
#if HAVE_WINDOWS
    static SDL_AtomicInt counter = {};
    for (u32 attempt = 0; attempt < 100; attempt++)
    {
        wchar_t wide[OS_MAX_PATH];
        const u32 unique = (u32)SDL_AddAtomicInt(&counter, 1);
        if (cstr_format(out_path, size, "%s\\%s_%u_%u_%llu", base, prefix ? prefix : "puppetxr", os_process_id(), unique, (unsigned long long)os_monotonic_ns()) >= size || !to_wide(out_path, wide, ARRAY_SIZE(wide)))
        {
            return false;
        }
        if (CreateDirectoryW(wide, nullptr))
        {
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return false;
        }
    }
    return false;
#else
    return cstr_format(out_path, size, "%s/%s_XXXXXX", base, prefix ? prefix : "puppetxr") < size && mkdtemp(out_path) != nullptr;
#endif
}

b8 os_current_directory (char* out_path, usize size)
{
    char* current = SDL_GetCurrentDirectory();
    if (!current)
    {
        return false;
    }
    const b8 fits = cstr_copy(out_path, size, current);
    SDL_free(current);
    return fits;
}

// On Windows a drive letter with a separator, or a separator first (UNC and rooted paths)
b8 os_path_is_absolute (const char* path)
{
    if (!path)
    {
        return false;
    }
#if HAVE_WINDOWS
    const b8 drive = ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    return drive || path[0] == '\\' || path[0] == '/';
#else
    return path[0] == '/';
#endif
}

#if HAVE_WINDOWS

b8 os_delete_file (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    return to_wide(path, wide, ARRAY_SIZE(wide)) && DeleteFileW(wide);
}

b8 os_create_directory (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    return to_wide(path, wide, ARRAY_SIZE(wide)) && (CreateDirectoryW(wide, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

b8 os_runtime_dir (char* out_path, usize size)
{
    return os_temp_dir(out_path, size);
}

// The per-user temporary directory, without its trailing separator
b8 os_temp_dir (char* out_path, usize size)
{
    wchar_t     wide[OS_MAX_PATH];
    const DWORD length = GetTempPathW(ARRAY_SIZE(wide), wide);
    if (length == 0 || length >= ARRAY_SIZE(wide))
    {
        return false;
    }
    if (wide[length - 1] == L'\\' || wide[length - 1] == L'/')
    {
        wide[length - 1] = 0;
    }
    return from_wide(wide, out_path, size);
}

b8 os_state_dir (char* out_path, usize size)
{
    const char* forced = os_env("PUPPETXR_STATE_DIR");
    if (forced && forced[0])
    {
        return cstr_format(out_path, size, "%s", forced) < size && os_create_directories(out_path);
    }
    const char* local = os_env("LOCALAPPDATA");
    return local && local[0] && cstr_format(out_path, size, "%s\\puppetxr", local) < size && os_create_directory(out_path);
}

u32 os_process_id ()
{
    return (u32)GetCurrentProcessId();
}

b8 os_process_alive (u32 pid)
{
    if (pid == 0)
    {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
    {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD code = 0;
    const b8 running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

// FIONBIO is an unsigned constant for a signed parameter
#define OS_FIONBIO ((long)FIONBIO)

// Winsock counts its users; every socket holds one start
static b8 winsock_start ()
{
    WSADATA data = {};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static b8 socket_address (const char* address, u16 port, sockaddr_in& out_address)
{
    out_address            = {};
    out_address.sin_family = AF_INET;
    out_address.sin_port   = htons(port);
    return address && inet_pton(AF_INET, address, &out_address.sin_addr) == 1;
}

// Not inherited by child processes, non-blocking, without Nagle's delay
static SOCKET socket_open ()
{
    if (!winsock_start())
    {
        return INVALID_SOCKET;
    }
    const SOCKET fd = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (fd == INVALID_SOCKET)
    {
        WSACleanup();
        return INVALID_SOCKET;
    }
    u_long non_blocking = 1;
    ioctlsocket(fd, OS_FIONBIO, &non_blocking);
    return fd;
}

static void socket_discard (SOCKET fd)
{
    closesocket(fd);
    WSACleanup();
}

static void socket_no_delay (SOCKET fd)
{
    const BOOL no_delay = TRUE;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&no_delay, sizeof(no_delay));
}

static OS_Socket* socket_wrap (SOCKET fd)
{
    OS_Socket*  socket = (OS_Socket*)os_memory_alloc(sizeof(OS_Socket));
    sockaddr_in bound  = {};
    int         length = sizeof(bound);
    socket->fd         = fd;
    socket->port       = getsockname(fd, (sockaddr*)&bound, &length) == 0 ? ntohs(bound.sin_port) : 0;
    return socket;
}

// select rather than WSAPoll, which misses failed connects before Windows 10 2004
static b8 socket_select (SOCKET fd, u32 timeout_ms, b8 for_writing, b8* out_failed)
{
    fd_set ready  = {};
    fd_set failed = {};
    FD_SET(fd, &ready);
    FD_SET(fd, &failed);
    const timeval timeout = { .tv_sec = (long)(timeout_ms / 1000), .tv_usec = (long)(timeout_ms % 1000 * 1000) };
    const int     count   = select(0, for_writing ? nullptr : &ready, for_writing ? &ready : nullptr, &failed, &timeout);
    if (out_failed)
    {
        *out_failed = count > 0 && FD_ISSET(fd, &failed);
    }
    return count > 0;
}

// SO_EXCLUSIVEADDRUSE: with SO_REUSEADDR Windows would let another process take the port over
OS_Socket* os_socket_listen (const char* address, u16 port)
{
    sockaddr_in addr = {};
    if (!socket_address(address, port, addr))
    {
        LOG_ERROR("Invalid listen address %s", address ? address : "(null)");
        return nullptr;
    }
    const SOCKET fd = socket_open();
    if (fd == INVALID_SOCKET)
    {
        return nullptr;
    }
    const BOOL exclusive = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0)
    {
        LOG_ERROR("Cannot listen on %s:%u: error %d", address, (u32)port, WSAGetLastError());
        socket_discard(fd);
        return nullptr;
    }
    return socket_wrap(fd);
}

u16 os_socket_port (const OS_Socket* socket)
{
    return socket ? socket->port : 0;
}

OS_Socket* os_socket_accept (const OS_Socket* listener)
{
    if (!listener || !winsock_start())
    {
        return nullptr;
    }
    const SOCKET fd = accept(listener->fd, nullptr, nullptr);
    if (fd == INVALID_SOCKET)
    {
        WSACleanup();
        return nullptr;
    }
    u_long non_blocking = 1;
    ioctlsocket(fd, OS_FIONBIO, &non_blocking);
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
    socket_no_delay(fd);
    return socket_wrap(fd);
}

OS_Socket* os_socket_connect (const char* address, u16 port, u32 timeout_ms)
{
    sockaddr_in addr = {};
    if (!socket_address(address, port, addr))
    {
        return nullptr;
    }
    const SOCKET fd = socket_open();
    if (fd == INVALID_SOCKET)
    {
        return nullptr;
    }
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        socket_discard(fd);
        return nullptr;
    }
    b8 failed = false;
    if (!socket_select(fd, timeout_ms, true, &failed) || failed)
    {
        socket_discard(fd);
        return nullptr;
    }
    socket_no_delay(fd);
    return socket_wrap(fd);
}

static b8 socket_would_block ()
{
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
}

OS_SocketResult os_socket_send (const OS_Socket* socket, const void* data, usize size, usize& out_sent)
{
    out_sent = 0;
    if (!socket)
    {
        return OS_SocketResult::Closed;
    }
    if (size == 0)
    {
        return OS_SocketResult::Ok;
    }
    const int chunk = size > 0x40000000 ? 0x40000000 : (int)size;
    const int sent  = send(socket->fd, (const char*)data, chunk, 0);
    if (sent == SOCKET_ERROR)
    {
        return socket_would_block() ? OS_SocketResult::WouldBlock : OS_SocketResult::Closed;
    }
    out_sent = (usize)sent;
    return OS_SocketResult::Ok;
}

OS_SocketResult os_socket_receive (const OS_Socket* socket, void* data, usize capacity, usize& out_received)
{
    out_received = 0;
    if (!socket || capacity == 0)
    {
        return OS_SocketResult::Closed;
    }
    const int chunk    = capacity > 0x40000000 ? 0x40000000 : (int)capacity;
    const int received = recv(socket->fd, (char*)data, chunk, 0);
    if (received == 0)
    {
        return OS_SocketResult::Closed;
    }
    if (received == SOCKET_ERROR)
    {
        return socket_would_block() ? OS_SocketResult::WouldBlock : OS_SocketResult::Closed;
    }
    out_received = (usize)received;
    return OS_SocketResult::Ok;
}

b8 os_socket_wait (const OS_Socket* socket, u32 timeout_ms, b8 for_writing)
{
    return socket && socket_select(socket->fd, timeout_ms, for_writing, nullptr);
}

void os_socket_close (OS_Socket* socket)
{
    if (socket)
    {
        socket_discard(socket->fd);
        os_memory_release(socket);
    }
}

// The holder keeps an exclusive byte range lock past the end of the file, which readers never
// touch and Windows drops when the holding process ends. The holder's handle has read access
// only, so plain readers that share nothing but reading can still open the file.
#define OS_LOCK_OFFSET_HIGH 0x7fffffffu

static b8 lock_range (HANDLE handle, b8 wait)
{
    OVERLAPPED at = {};
    at.OffsetHigh = OS_LOCK_OFFSET_HIGH;
    return LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &at) != 0;
}

static void unlock_range (HANDLE handle)
{
    OVERLAPPED at = {};
    at.OffsetHigh = OS_LOCK_OFFSET_HIGH;
    UnlockFileEx(handle, 0, 1, 0, &at);
}

// Written under a temporary name and renamed once locked, so a reader never sees the final
// name without a holder. Until the holder has its lock, the writer shares no deletion, so a
// sweep of stale files by another process cannot delete the temporary file under it; the holder
// waits for the lock, which os_lock_file_is_stale takes for a moment to probe.
// One try; false with `out_swept` when a sweep deleted the temporary file before the rename
static OS_LockFile* create_lock_file (const char* path, const char* temporary, const wchar_t* wide_temporary, const wchar_t* wide_path, const void* data, usize size, b8& out_swept)
{
    out_swept = false;
    const DWORD  share  = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE writer = CreateFileW(wide_temporary, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("Cannot create %s: error %lu", temporary, GetLastError());
        return nullptr;
    }
    const HANDLE holder  = ReOpenFile(writer, GENERIC_READ, share, 0);
    DWORD        written = 0;
    const b8     filled  = holder != INVALID_HANDLE_VALUE && lock_range(holder, true) && (size == 0 || (WriteFile(writer, data, (DWORD)size, &written, nullptr) && written == size && FlushFileBuffers(writer)));
    CloseHandle(writer);
    // A reader holding the last file of this path (deleted, or not shareable for a rename) makes
    // the rename fail until it closes it, which takes microseconds
    b8 moved = filled && MoveFileExW(wide_temporary, wide_path, MOVEFILE_REPLACE_EXISTING);
    for (u32 attempt = 0; filled && !moved && attempt < 200 && (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_SHARING_VIOLATION); attempt++)
    {
        Sleep(1);
        moved = MoveFileExW(wide_temporary, wide_path, MOVEFILE_REPLACE_EXISTING);
    }
    if (!moved)
    {
        const DWORD error = GetLastError();
        out_swept         = filled && error == ERROR_FILE_NOT_FOUND;
        if (!out_swept)
        {
            LOG_ERROR("Cannot write lock file %s: error %lu", path, error);
        }
        if (holder != INVALID_HANDLE_VALUE)
        {
            CloseHandle(holder);
        }
        DeleteFileW(wide_temporary);
        return nullptr;
    }
    OS_LockFile* file = (OS_LockFile*)os_memory_alloc(sizeof(OS_LockFile));
    file->handle      = holder;
    cstr_copy(file->path, sizeof(file->path), path);
    return file;
}

// A sweep that judged the temporary file stale just before the writer locked it may delete it once
// the writer let go of it and before the rename; that takes a new try
OS_LockFile* os_lock_file_create (const char* path, const void* data, usize size)
{
    char    temporary[OS_MAX_PATH];
    wchar_t wide_temporary[OS_MAX_PATH];
    wchar_t wide_path[OS_MAX_PATH];
    if (!path || cstr_format(temporary, sizeof(temporary), "%s.%u.tmp", path, os_process_id()) >= sizeof(temporary) || !to_wide(temporary, wide_temporary, ARRAY_SIZE(wide_temporary)) || !to_wide(path, wide_path, ARRAY_SIZE(wide_path)))
    {
        return nullptr;
    }
    b8 swept = true;
    for (u32 attempt = 0; swept && attempt < 8; attempt++)
    {
        if (OS_LockFile* file = create_lock_file(path, temporary, wide_temporary, wide_path, data, size, swept))
        {
            return file;
        }
    }
    if (swept)
    {
        LOG_ERROR("Cannot write lock file %s: swept away %u times", path, 8u);
    }
    return nullptr;
}

// Deleted while still held, so nobody sees the file without a holder
void os_lock_file_release (OS_LockFile* file)
{
    if (file)
    {
        os_delete_file(file->path);
        unlock_range(file->handle);
        CloseHandle(file->handle);
        os_memory_release(file);
    }
}

b8 os_lock_file_is_stale (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    if (!to_wide(path, wide, ARRAY_SIZE(wide)))
    {
        return false;
    }
    const HANDLE handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const b8 stale = lock_range(handle, false);
    if (stale)
    {
        unlock_range(handle);
    }
    CloseHandle(handle);
    return stale;
}

#else

b8 os_delete_file (const char* path)
{
    return path && unlink(path) == 0;
}

b8 os_create_directory (const char* path)
{
    return path && (mkdir(path, 0700) == 0 || errno == EEXIST);
}

// $XDG_RUNTIME_DIR is per user, private and emptied at logout; /tmp is the fallback
b8 os_runtime_dir (char* out_path, usize size)
{
    const char* runtime = os_env("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] && os_directory_exists(runtime))
    {
        return cstr_format(out_path, size, "%s", runtime) < size;
    }
    return cstr_format(out_path, size, "/tmp") < size;
}

b8 os_temp_dir (char* out_path, usize size)
{
    const char* temporary = os_env("TMPDIR");
    return cstr_format(out_path, size, "%s", temporary && temporary[0] ? temporary : "/tmp") < size;
}

// $PUPPETXR_STATE_DIR, else the XDG Base Directory spec: $XDG_STATE_HOME if absolute, else ~/.local/state
b8 os_state_dir (char* out_path, usize size)
{
    const char* forced = os_env("PUPPETXR_STATE_DIR");
    if (forced && forced[0])
    {
        return cstr_format(out_path, size, "%s", forced) < size && os_create_directories(out_path);
    }
    const char* state = os_env("XDG_STATE_HOME");
    char        base[OS_MAX_PATH];
    if (state && state[0] == '/')
    {
        if (cstr_format(base, sizeof(base), "%s", state) >= sizeof(base) || !os_create_directory(base))
        {
            return false;
        }
    }
    else
    {
        const char* home = os_env("HOME");
        if (!home || home[0] != '/')
        {
            return false;
        }
        char local[OS_MAX_PATH];
        if (cstr_format(local, sizeof(local), "%s/.local", home) >= sizeof(local) || !os_create_directory(local))
        {
            return false;
        }
        if (cstr_format(base, sizeof(base), "%s/.local/state", home) >= sizeof(base) || !os_create_directory(base))
        {
            return false;
        }
    }
    return cstr_format(out_path, size, "%s/puppetxr", base) < size && os_create_directory(out_path);
}

u32 os_process_id ()
{
    return (u32)getpid();
}

b8 os_process_alive (u32 pid)
{
    if (pid == 0 || pid > 0x7FFFFFFFu)
    {
        return false;
    }
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

static b8 socket_address (const char* address, u16 port, sockaddr_in& out_address)
{
    out_address            = {};
    out_address.sin_family = AF_INET;
    out_address.sin_port   = htons(port);
    return address && inet_pton(AF_INET, address, &out_address.sin_addr) == 1;
}

static b8 socket_would_block (int error)
{
#if EAGAIN != EWOULDBLOCK
    if (error == EWOULDBLOCK)
    {
        return true;
    }
#endif
    return error == EAGAIN || error == EINTR;
}

// macOS has no SOCK_NONBLOCK, SOCK_CLOEXEC, accept4 or MSG_NOSIGNAL; the flags are set on the fd
static i32 socket_open_stream ()
{
#if HAVE_MACOS
    const i32 fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return fd;
    }
    const i32 no_sigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
#else
    return socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
}

static i32 socket_accept_stream (i32 listener)
{
#if HAVE_MACOS
    const i32 fd = accept(listener, nullptr, nullptr);
    if (fd < 0)
    {
        return fd;
    }
    const i32 no_sigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
#else
    return accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
}

static OS_Socket* socket_wrap (i32 fd)
{
    OS_Socket*  socket = (OS_Socket*)os_memory_alloc(sizeof(OS_Socket));
    sockaddr_in bound  = {};
    socklen_t   length = sizeof(bound);
    socket->fd         = fd;
    socket->port       = getsockname(fd, (sockaddr*)&bound, &length) == 0 ? ntohs(bound.sin_port) : 0;
    return socket;
}

OS_Socket* os_socket_listen (const char* address, u16 port)
{
    sockaddr_in addr = {};
    if (!socket_address(address, port, addr))
    {
        LOG_ERROR("Invalid listen address %s", address ? address : "(null)");
        return nullptr;
    }
    const i32 fd = socket_open_stream();
    if (fd < 0)
    {
        return nullptr;
    }
    const i32 reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0)
    {
        LOG_ERROR("Cannot listen on %s:%u: %s", address, (u32)port, errno_text(errno).text);
        close(fd);
        return nullptr;
    }
    return socket_wrap(fd);
}

u16 os_socket_port (const OS_Socket* socket)
{
    return socket ? socket->port : 0;
}

OS_Socket* os_socket_accept (const OS_Socket* listener)
{
    if (!listener)
    {
        return nullptr;
    }
    const i32 fd = socket_accept_stream(listener->fd);
    if (fd < 0)
    {
        return nullptr;
    }
    const i32 no_delay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    return socket_wrap(fd);
}

OS_Socket* os_socket_connect (const char* address, u16 port, u32 timeout_ms)
{
    sockaddr_in addr = {};
    if (!socket_address(address, port, addr))
    {
        return nullptr;
    }
    const i32 fd = socket_open_stream();
    if (fd < 0)
    {
        return nullptr;
    }
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0 && errno != EINPROGRESS)
    {
        close(fd);
        return nullptr;
    }
    pollfd    waiting = { .fd = fd, .events = POLLOUT, .revents = 0 };
    i32       error   = 0;
    socklen_t length  = sizeof(error);
    if (poll(&waiting, 1, (int)timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
    {
        close(fd);
        return nullptr;
    }
    const i32 no_delay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    return socket_wrap(fd);
}

OS_SocketResult os_socket_send (const OS_Socket* socket, const void* data, usize size, usize& out_sent)
{
    out_sent = 0;
    if (!socket)
    {
        return OS_SocketResult::Closed;
    }
    if (size == 0)
    {
        return OS_SocketResult::Ok;
    }
    #if HAVE_MACOS
    const i32 send_flags = 0;
#else
    const i32 send_flags = MSG_NOSIGNAL;
#endif
    const ssize_t sent = send(socket->fd, data, size, send_flags);
    if (sent < 0)
    {
        return socket_would_block(errno) ? OS_SocketResult::WouldBlock : OS_SocketResult::Closed;
    }
    out_sent = (usize)sent;
    return OS_SocketResult::Ok;
}

OS_SocketResult os_socket_receive (const OS_Socket* socket, void* data, usize capacity, usize& out_received)
{
    out_received = 0;
    if (!socket || capacity == 0)
    {
        return OS_SocketResult::Closed;
    }
    const ssize_t received = recv(socket->fd, data, capacity, 0);
    if (received == 0)
    {
        return OS_SocketResult::Closed;
    }
    if (received < 0)
    {
        return socket_would_block(errno) ? OS_SocketResult::WouldBlock : OS_SocketResult::Closed;
    }
    out_received = (usize)received;
    return OS_SocketResult::Ok;
}

b8 os_socket_wait (const OS_Socket* socket, u32 timeout_ms, b8 for_writing)
{
    if (!socket)
    {
        return false;
    }
    pollfd waiting = { .fd = socket->fd, .events = (short)(for_writing ? POLLOUT : POLLIN), .revents = 0 };
    return poll(&waiting, 1, (int)timeout_ms) == 1;
}

void os_socket_close (OS_Socket* socket)
{
    if (socket)
    {
        close(socket->fd);
        os_memory_release(socket);
    }
}

// Written under a temporary name and renamed once locked, so a reader never sees the final
// name without a holder
OS_LockFile* os_lock_file_create (const char* path, const void* data, usize size)
{
    char temporary[OS_MAX_PATH];
    if (!path || cstr_format(temporary, sizeof(temporary), "%s.%u.tmp", path, os_process_id()) >= sizeof(temporary))
    {
        return nullptr;
    }
    const i32 fd = open(temporary, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        LOG_ERROR("Cannot create %s: %s", temporary, errno_text(errno).text);
        return nullptr;
    }
    const b8 written = flock(fd, LOCK_EX | LOCK_NB) == 0 && (size == 0 || write(fd, data, size) == (ssize_t)size) && rename(temporary, path) == 0;
    if (!written)
    {
        LOG_ERROR("Cannot write lock file %s: %s", path, errno_text(errno).text);
        unlink(temporary);
        close(fd);
        return nullptr;
    }
    OS_LockFile* file = (OS_LockFile*)os_memory_alloc(sizeof(OS_LockFile));
    file->fd          = fd;
    cstr_copy(file->path, sizeof(file->path), path);
    return file;
}

void os_lock_file_release (OS_LockFile* file)
{
    if (file)
    {
        unlink(file->path);
        close(file->fd);
        os_memory_release(file);
    }
}

b8 os_lock_file_is_stale (const char* path)
{
    if (!path)
    {
        return false;
    }
    const i32 fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    const b8 stale = flock(fd, LOCK_EX | LOCK_NB) == 0;
    close(fd);
    return stale;
}

#endif

