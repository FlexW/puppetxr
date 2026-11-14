#include "platform_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

//
// Types
//

#define OS_WINDOW_EVENT_QUEUE 512
#define OS_MAX_POSTED_CALLS   256
#define OS_WINDOW_WAIT_NS     100000000ull // the window thread's longest sleep; a wake ends it sooner

namespace
{

struct PostedCall
{
    OS_WindowThreadFunction* function;
    void*                    user;
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
    u32             pushed_window;      // got synthetic mouse events, which then drive its mouse alone
    u32             hook_motion_window; // where the hook's last mouse motion was, to give clicks their own
    f32             hook_motion_x;
    f32             hook_motion_y;

    OS_Mutex*       log_mutex;
    OS_LogCallback* log_callback;
    void*           log_callback_user;
    OS_File*        log_file;      // PUPPETXR_LOG_FILE, opened on first use
    b8              log_file_tried;
    OS_LogLine      log_lines[OS_LOG_HISTORY];
    u32             log_head;
    u32             log_count;
    u64             log_sequence;
    u64             start_ns;      // log times count from here

    b8              capture_active;
    i32             capture_read;     // pipe read end
    i32             saved_stdout;     // originals, written through; -1 when the process had none
    i32             saved_stderr;
    char            capture_partial[OS_LOG_LINE_LENGTH];
    u32             capture_partial_length;

    // Video: the driver, its windows and what they reported, for os_poll_events
    const OS_VideoDriver* driver;
    char            requested_driver[32]; // $PUPPETXR_VIDEO_DRIVER at os_initialize
    OS_Window       windows[OS_MAX_WINDOWS];
    u32             next_window_id;
    OS_Mutex*       queue_mutex;
    OS_RawEvent*    queue;            // OS_WINDOW_EVENT_QUEUE entries, a ring
    u32             queue_head;
    u32             queue_count;

#if HAVE_WINDOW_THREAD
    OS_Thread*      window_thread;
    OS_Atomic32     window_thread_id_set;
    u64             window_thread_id;
    OS_Semaphore*   window_ready;
    b8              window_thread_ok;
    OS_Atomic32     window_quit;
    OS_Mutex*       call_mutex;       // one os_run_on_window_thread call at a time
    OS_Mutex*       call_slot;        // the waiting call; whoever takes it runs it
    OS_Semaphore*   call_done;
    OS_WindowThreadFunction* call_function;
    void*           call_user;
    i32             call_tickets;
    i32             call_ticket;      // of the waiting call, 0 when none
    OS_Semaphore*   window_wake;      // ends the wait of a window thread waiting in os_mutex_lock
    OS_Atomic32     window_waiting;
    OS_Mutex*       posted_mutex;
    PostedCall      posted[OS_MAX_POSTED_CALLS];
    u32             posted_head;
    u32             posted_count;
    b8              posted_full_logged;
#endif
};

enum class WindowOp : u8
{
    Create,
    Destroy,
    NativeHandles,
    Size,
    Density,
    RefreshRate,
    VulkanExtensions,
    CreateVulkanSurface,
    MetalLayer,
    RelativeMouse,
    Maximize,
    Maximized,
    Show,
    Hide,
    Shown,
    CursorPosition,
    KeepCursor,
    SetCursor,
    TextInput,
    ClipboardText,
    SetClipboardText,
    SetVideoDriver,
    KeyLabel,
};

// Arguments and results of a window function, run on the video thread
struct WindowCall
{
    WindowOp             op;
    OS_Window*           window;
    const OS_WindowDesc* desc;
    OS_WindowNative*     native;
    void*                vk_instance;
    const void*          vk_get_instance_proc_addr;
    u64                  surface;
    const char*          text;
    char*                out_text;
    usize                size;
    f32                  x;
    f32                  y;
    f32                  height;
    u32                  width_px;
    u32                  height_px;
    f32                  value;
    b8                   flag;
    b8                   result;
    void*                pointer;
    const char* const*   names;
    u32                  count;
    OS_Cursor            cursor;
    OS_Key               key;
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
    "insert", "delete", "home", "end", "page_up", "page_down",
    "minus", "equals", "backslash", "semicolon", "apostrophe", "comma", "period", "slash", "non_us_backslash",
    "caps_lock", "scroll_lock", "num_lock", "print_screen", "pause", "menu", "left_super", "right_super",
    "keypad_0", "keypad_1", "keypad_2", "keypad_3", "keypad_4", "keypad_5", "keypad_6", "keypad_7", "keypad_8", "keypad_9",
    "keypad_decimal", "keypad_divide", "keypad_multiply", "keypad_minus", "keypad_plus", "keypad_enter", "keypad_equals",
    "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24",
    "mouse_x1", "mouse_x2",
};

// What a US layout prints on each key, for os_key_label where the driver knows no layout
static const char* const os_key_ascii_labels[(u32)OS_Key::Count] = {
    "",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Space", "Escape", "Return", "Tab", "Backspace",
    "Left", "Right", "Up", "Down",
    "Left Shift", "Left Ctrl", "Left Alt", "Right Shift", "Right Ctrl", "Right Alt",
    "", "", "",
    "`", "[", "]",
    "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
    "-", "=", "\\", ";", "'", ",", ".", "/", "\\",
    "CapsLock", "ScrollLock", "Numlock", "PrintScreen", "Pause", "Menu", "Left GUI", "Right GUI",
    "Keypad 0", "Keypad 1", "Keypad 2", "Keypad 3", "Keypad 4", "Keypad 5", "Keypad 6", "Keypad 7", "Keypad 8", "Keypad 9",
    "Keypad .", "Keypad /", "Keypad *", "Keypad -", "Keypad +", "Keypad Enter", "Keypad =",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "", "",
};

#if HAVE_LINUX
static const OS_VideoDriver* const os_auto_drivers[] = { &os_wayland_driver, &os_x11_driver };
#elif HAVE_WINDOWS
static const OS_VideoDriver* const os_auto_drivers[] = { &os_windows_driver };
#elif HAVE_MACOS
static const OS_VideoDriver* const os_auto_drivers[] = { &os_cocoa_driver };
#endif

static OS_State os_state = {
    .log_level   = OS_LogLevel::Info,
    .assert_mode = PUPPETXR_DIST ? OS_AssertMode::Log : OS_AssertMode::Break,
};

static usize zm_min_usize (usize a, usize b)
{
    return a < b ? a : b;
}

static void log_append (OS_LogLevel level, OS_LogSource source, const char* text)
{
    if (!os_state.log_mutex)
    {
        os_state.log_mutex = os_mutex_create();
        os_state.start_ns  = os_monotonic_ns();
    }
    os_sys_mutex_lock(os_state.log_mutex);
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
    line.time_ns = os_time_ns();
    cstr_copy(line.text, sizeof(line.text), text);
    os_state.log_sequence++;

    // Hosts like test frameworks and engines often swallow stdout and stderr
    if (!os_state.log_file_tried)
    {
        os_state.log_file_tried = true;
        const char* path        = os_env("PUPPETXR_LOG_FILE");
        os_state.log_file       = path && path[0] ? os_file_append(path) : nullptr;
    }
    if (os_state.log_file)
    {
        static const char* const level_names[] = { "debug", "info", "warning", "error" };
        const char*              origin        = source == OS_LogSource::App ? "app" : level_names[(u32)level];
        char                     prefix[32];
        const usize              length = cstr_format(prefix, sizeof(prefix), "%10.3f %-7s ", (f64)line.time_ns * 1e-9, origin);
        os_file_write(os_state.log_file, prefix, zm_min_usize(length, sizeof(prefix) - 1));
        os_file_write(os_state.log_file, text, cstr_length(text));
        os_file_write(os_state.log_file, "\n", 1);
        os_file_flush(os_state.log_file);
    }
    OS_LogCallback* const callback = source == OS_LogSource::Sim ? os_state.log_callback : nullptr;
    void* const           user     = os_state.log_callback_user;
    os_sys_mutex_unlock(os_state.log_mutex);

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

//
// Lifecycle
//

static const OS_VideoDriver* find_driver (const char* name)
{
    if (cstr_equal(name, os_offscreen_driver.name))
    {
        return &os_offscreen_driver;
    }
    for (const OS_VideoDriver* driver : os_auto_drivers)
    {
        if (cstr_equal(name, driver->name))
        {
            return driver;
        }
    }
    return nullptr;
}

// Video thread. A named driver is the only one tried; otherwise the first of the platform's that
// finds a display. The offscreen driver is only ever named.
static b8 video_start (const char* name)
{
    if (name && name[0])
    {
        const OS_VideoDriver* driver = find_driver(name);
        if (!driver)
        {
            LOG_WARNING("No video driver named %s", name);
            return false;
        }
        if (!driver->init())
        {
            return false;
        }
        os_state.driver = driver;
        return true;
    }
    for (const OS_VideoDriver* driver : os_auto_drivers)
    {
        if (driver->init())
        {
            os_state.driver = driver;
            return true;
        }
    }
    return false;
}

static void video_stop ()
{
    if (!os_state.driver)
    {
        return;
    }
    for (OS_Window& window : os_state.windows)
    {
        if (window.driver)
        {
            window.driver->destroy_window(&window);
            window = {};
        }
    }
    os_state.driver->quit();
    os_state.driver = nullptr;
}

#if HAVE_WINDOW_THREAD
static b8 on_window_thread ()
{
    return os_state.window_thread && os_atomic_load(&os_state.window_thread_id_set) && os_thread_current_id() == os_state.window_thread_id;
}

static void wake_window_thread ()
{
    os_sys_wake_signal();
    if (os_atomic_load(&os_state.window_waiting) > 0)
    {
        os_semaphore_signal(os_state.window_wake);
    }
}

// Runs the waiting call if it is `ticket`'s, any waiting call for 0
static void run_window_thread_call (i32 ticket)
{
    os_sys_mutex_lock(os_state.call_slot);
    const b8                       taken    = os_state.call_ticket && (!ticket || ticket == os_state.call_ticket);
    OS_WindowThreadFunction* const function = os_state.call_function;
    void* const                    user     = os_state.call_user;
    if (taken)
    {
        os_state.call_ticket = 0;
    }
    os_sys_mutex_unlock(os_state.call_slot);
    if (taken)
    {
        function(user);
        os_semaphore_signal(os_state.call_done);
    }
}

// Posted calls run in the order they came
static void run_posted_calls ()
{
    for (;;)
    {
        os_sys_mutex_lock(os_state.posted_mutex);
        const b8         any  = os_state.posted_count > 0;
        const PostedCall call = any ? os_state.posted[os_state.posted_head] : PostedCall {};
        if (any)
        {
            os_state.posted_head = (os_state.posted_head + 1) % OS_MAX_POSTED_CALLS;
            os_state.posted_count--;
        }
        os_sys_mutex_unlock(os_state.posted_mutex);
        if (!any)
        {
            return;
        }
        call.function(call.user);
    }
}

static b8 window_thread_idle ()
{
    os_sys_mutex_lock(os_state.call_slot);
    const b8 call = os_state.call_ticket != 0;
    os_sys_mutex_unlock(os_state.call_slot);
    os_sys_mutex_lock(os_state.posted_mutex);
    const b8 posted = os_state.posted_count > 0;
    os_sys_mutex_unlock(os_state.posted_mutex);
    return !call && !posted && !os_atomic_load(&os_state.window_quit);
}

static i32 window_thread_main (void* user) // NOLINT(misc-const-correctness): OS_ThreadFunction
{
    (void)user;
    os_state.window_thread_id = os_thread_current_id();
    os_atomic_store(&os_state.window_thread_id_set, 1);
    os_state.window_thread_ok = video_start(os_state.requested_driver);
    os_semaphore_signal(os_state.window_ready);
    if (!os_state.window_thread_ok)
    {
        return 0;
    }
    while (!os_atomic_load(&os_state.window_quit))
    {
        run_window_thread_call(0);
        run_posted_calls();
        os_state.driver->pump();
        if (window_thread_idle())
        {
            os_state.driver->wait(OS_WINDOW_WAIT_NS);
        }
    }
    run_posted_calls();
    video_stop();
    return 0;
}

static b8 start_window_thread ()
{
    os_sys_wake_open();
    os_state.window_ready = os_semaphore_create(0);
    os_state.call_mutex   = os_mutex_create();
    os_state.call_slot    = os_mutex_create();
    os_state.call_done    = os_semaphore_create(0);
    os_state.window_wake  = os_semaphore_create(0);
    os_state.posted_mutex = os_mutex_create();
    os_state.call_ticket  = 0;
    os_state.posted_head  = 0;
    os_state.posted_count = 0;
    os_atomic_store(&os_state.window_quit, 0);
    os_atomic_store(&os_state.window_thread_id_set, 0);
    os_state.window_thread = os_thread_create("puppetxr window", window_thread_main, nullptr);
    if (os_state.window_thread)
    {
        os_semaphore_wait(os_state.window_ready);
    }
    if (os_state.window_thread && !os_state.window_thread_ok)
    {
        os_thread_join(os_state.window_thread);
        os_state.window_thread = nullptr;
        LOG_WARNING("No display available, continuing headless");
    }
    return os_state.window_thread != nullptr;
}

static void stop_window_thread ()
{
    if (os_state.window_thread)
    {
        os_atomic_store(&os_state.window_quit, 1);
        wake_window_thread();
        os_thread_join(os_state.window_thread);
        os_state.window_thread = nullptr;
    }
    os_semaphore_destroy(os_state.window_ready);
    os_semaphore_destroy(os_state.call_done);
    os_semaphore_destroy(os_state.window_wake);
    os_mutex_destroy(os_state.call_mutex);
    os_mutex_destroy(os_state.call_slot);
    os_mutex_destroy(os_state.posted_mutex);
    os_state.window_ready = nullptr;
    os_state.call_done    = nullptr;
    os_state.window_wake  = nullptr;
    os_state.call_mutex   = nullptr;
    os_state.call_slot    = nullptr;
    os_state.posted_mutex = nullptr;
    os_sys_wake_close();
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

static void init_video_now (void* user)
{
    ((VideoInitCall*)user)->ok = video_start(os_state.requested_driver);
}

static void stop_video_now (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    (void)user;
    video_stop();
}
#endif

b8 os_initialize (b8 headless)
{
    if (os_state.initialized)
    {
        return true;
    }
    (void)os_time_ns();
    os_state.queue       = (OS_RawEvent*)os_memory_alloc(sizeof(OS_RawEvent) * OS_WINDOW_EVENT_QUEUE);
    os_state.queue_mutex = os_mutex_create();
    os_state.queue_head  = 0;
    os_state.queue_count = 0;
    const char* requested = os_env("PUPPETXR_VIDEO_DRIVER");
    cstr_copy(os_state.requested_driver, sizeof(os_state.requested_driver), requested ? requested : "");

#if HAVE_WINDOW_THREAD
    if (!headless && !start_window_thread())
    {
        stop_window_thread();
        headless = true;
    }
#elif HAVE_MACOS
    // Cocoa starts on the main thread only, which may be busy or blocked on the caller
    os_mac_queue_create();
    if (!headless)
    {
        VideoInitCall call = {};
        if (os_mac_on_main_thread())
        {
            init_video_now(&call);
        }
        else if (!os_mac_run_on_main(init_video_now, &call, 2000))
        {
            LOG_WARNING("The main thread does not take window calls, continuing headless");
            headless = true;
        }
        if (!headless && !call.ok)
        {
            LOG_WARNING("No display available, continuing headless");
            headless = true;
        }
    }
#endif

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
#elif HAVE_MACOS
    if (!os_state.headless)
    {
        os_run_on_window_thread(stop_video_now, nullptr);
    }
#endif
    os_mutex_destroy(os_state.queue_mutex);
    os_memory_release(os_state.queue);
    os_state.queue_mutex   = nullptr;
    os_state.queue         = nullptr;
    os_state.queue_count   = 0;
    os_state.initialized   = false;
    os_state.headless      = false;
    os_state.pushed_window = 0;
}

b8 os_is_window_thread ()
{
    if (os_state.headless || !os_state.initialized)
    {
        return true;
    }
#if HAVE_WINDOW_THREAD
    return !os_state.window_thread || on_window_thread();
#elif HAVE_MACOS
    return os_mac_on_main_thread();
#else
    return true;
#endif
}

void os_run_on_window_thread (OS_WindowThreadFunction* function, void* user)
{
    ASSERT(function);
#if HAVE_WINDOW_THREAD
    if (os_state.window_thread && !on_window_thread())
    {
        os_sys_mutex_lock(os_state.call_mutex);
        os_state.call_tickets = os_state.call_tickets == INT32_MAX ? 1 : os_state.call_tickets + 1;
        const i32 ticket      = os_state.call_tickets;
        os_sys_mutex_lock(os_state.call_slot);
        ASSERT(!os_state.call_ticket);
        os_state.call_function = function;
        os_state.call_user     = user;
        os_state.call_ticket   = ticket;
        os_sys_mutex_unlock(os_state.call_slot);
        // Woken again until it ran: the window thread may also wait for a mutex this thread holds
        // (os_mutex_lock), and take the call there
        do
        {
            wake_window_thread();
        } while (!os_sys_semaphore_wait_ns(os_state.call_done, 1000000ull));
        os_sys_mutex_unlock(os_state.call_mutex);
        return;
    }
#elif HAVE_MACOS
    if (!os_is_window_thread())
    {
        os_mac_run_on_main(function, user, OS_MAIN_THREAD_TIMEOUT_MS);
        return;
    }
#endif
    function(user);
}

void os_post_to_window_thread (OS_WindowThreadFunction* function, void* user)
{
    ASSERT(function);
    if (os_is_window_thread())
    {
        function(user);
        return;
    }
#if HAVE_WINDOW_THREAD
    os_sys_mutex_lock(os_state.posted_mutex);
    const b8 queued = os_state.posted_count < OS_MAX_POSTED_CALLS;
    if (queued)
    {
        os_state.posted[(os_state.posted_head + os_state.posted_count) % OS_MAX_POSTED_CALLS] = { .function = function, .user = user };
        os_state.posted_count++;
    }
    const b8 log_full = !queued && !os_state.posted_full_logged;
    os_state.posted_full_logged = os_state.posted_full_logged || log_full;
    os_sys_mutex_unlock(os_state.posted_mutex);
    if (log_full)
    {
        LOG_WARNING("%u calls wait for the window thread; dropping more", OS_MAX_POSTED_CALLS);
    }
    wake_window_thread();
#elif HAVE_MACOS
    os_mac_post_to_main(function, user);
#endif
}

#if HAVE_WINDOW_THREAD
static void barrier_call (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    (void)user;
}
#endif

void os_serve_posted (u64 ns)
{
#if HAVE_MACOS
    if (os_mac_on_main_thread())
    {
        os_mac_serve(ns);
        return;
    }
#elif HAVE_WINDOW_THREAD
    // Posted calls run before a blocking call, so one waits them out
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
        os_mutex_destroy(os_state.log_mutex);
        os_state.log_mutex = nullptr;
    }
}

b8 os_is_headless ()
{
    return os_state.headless;
}

//
// Time and files
//

// Counts from the first call, which os_initialize or the first log line makes
u64 os_time_ns ()
{
    static const u64 start = os_monotonic_ns();
    return os_monotonic_ns() - start;
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

static OS_Window* add_window ()
{
    for (OS_Window& window : os_state.windows)
    {
        if (!window.driver)
        {
            window    = {};
            os_state.next_window_id = os_state.next_window_id == UINT32_MAX ? 1 : os_state.next_window_id + 1;
            window.id = os_state.next_window_id;
            return &window;
        }
    }
    LOG_ERROR("More than %u windows", OS_MAX_WINDOWS);
    return nullptr;
}

OS_Window* os_window_of_id (u32 id)
{
    for (OS_Window& window : os_state.windows)
    {
        if (window.driver && window.id == id)
        {
            return &window;
        }
    }
    return nullptr;
}

OS_Window* os_window_at (u32 index)
{
    return index < OS_MAX_WINDOWS && os_state.windows[index].driver ? &os_state.windows[index] : nullptr;
}

const char* os_key_ascii_label (OS_Key key)
{
    return (u32)key < (u32)OS_Key::Count ? os_key_ascii_labels[(u32)key] : "";
}

static void set_video_driver_now (WindowCall& call)
{
    if (os_state.driver && cstr_equal(os_state.driver->name, call.text))
    {
        call.result = true;
        return;
    }
    const OS_VideoDriver* next = find_driver(call.text);
    if (!next)
    {
        LOG_WARNING("Video driver %s is not available (no such driver)", call.text);
        return;
    }
    const OS_VideoDriver* previous = os_state.driver;
    video_stop();
    if (next->init())
    {
        os_state.driver = next;
        call.result     = true;
        return;
    }
    LOG_WARNING("Video driver %s is not available", call.text);
    if (previous && previous->init())
    {
        os_state.driver = previous;
    }
    else if (previous)
    {
        LOG_ERROR("Cannot restart video on %s", previous->name);
    }
}

static void create_window_now (WindowCall& call)
{
    OS_Window* window = os_state.driver ? add_window() : nullptr;
    if (!window)
    {
        return;
    }
    window->driver  = os_state.driver;
    window->density = 1.0f;
    window->cursor  = OS_Cursor::Arrow;
    if (!os_state.driver->create_window(window, *call.desc))
    {
        *window = {};
        return;
    }
    call.window = window;
}

static void window_call_now (void* user)
{
    WindowCall&           call   = *(WindowCall*)user;
    OS_Window* const      window = call.window;
    const OS_VideoDriver* driver = window ? window->driver : os_state.driver;
    if (call.op != WindowOp::Create && call.op != WindowOp::SetVideoDriver && !driver)
    {
        return;
    }
    switch (call.op)
    {
    case WindowOp::Create:
        create_window_now(call);
        break;
    case WindowOp::Destroy:
        driver->destroy_window(window);
        *window = {};
        break;
    case WindowOp::NativeHandles:
        call.result = driver->native_handles && driver->native_handles(window, *call.native);
        break;
    case WindowOp::Size:
        call.width_px  = window->width;
        call.height_px = window->height;
        break;
    case WindowOp::Density:
        call.value = window->density;
        break;
    case WindowOp::RefreshRate:
        call.value = driver->refresh_rate ? driver->refresh_rate(window) : 0.0f;
        break;
    case WindowOp::VulkanExtensions:
        call.names = driver->vulkan_extensions ? driver->vulkan_extensions(call.count) : nullptr;
        break;
    case WindowOp::CreateVulkanSurface:
        call.result = driver->create_vulkan_surface && driver->create_vulkan_surface(window, call.vk_instance, call.vk_get_instance_proc_addr, call.surface);
        break;
    case WindowOp::MetalLayer:
        call.pointer = driver->metal_layer ? driver->metal_layer(window) : nullptr;
        break;
    case WindowOp::RelativeMouse:
        if (driver->set_relative_mouse && window->relative != call.flag)
        {
            driver->set_relative_mouse(window, call.flag);
        }
        window->relative = call.flag;
        break;
    case WindowOp::Maximize:
        call.result = driver->maximize && driver->maximize(window);
        break;
    case WindowOp::Maximized:
        call.result = driver->maximized && driver->maximized(window);
        break;
    case WindowOp::Show:
        if (driver->show)
        {
            driver->show(window);
        }
        window->hidden = false;
        break;
    case WindowOp::Hide:
        if (driver->hide)
        {
            driver->hide(window);
        }
        window->hidden = true;
        break;
    case WindowOp::Shown:
        call.result = !window->hidden;
        break;
    case WindowOp::CursorPosition:
        call.result = driver->cursor_position && window->id != os_state.pushed_window && !window->relative && driver->cursor_position(window, call.x, call.y);
        break;
    case WindowOp::KeepCursor:
        if (driver->keep_cursor)
        {
            driver->keep_cursor(window, call.flag);
        }
        break;
    case WindowOp::SetCursor:
        if (window->cursor != call.cursor && driver->set_cursor)
        {
            driver->set_cursor(window, call.cursor);
        }
        window->cursor = call.cursor;
        break;
    case WindowOp::TextInput:
        if (driver->text_input)
        {
            driver->text_input(window, call.flag, call.x, call.y, call.height);
        }
        break;
    case WindowOp::ClipboardText:
        call.result = driver->clipboard_text && driver->clipboard_text(window, call.out_text, call.size);
        break;
    case WindowOp::SetClipboardText:
        if (driver->set_clipboard_text)
        {
            driver->set_clipboard_text(window, call.text);
        }
        break;
    case WindowOp::SetVideoDriver:
        set_video_driver_now(call);
        break;
    case WindowOp::KeyLabel:
        call.result = driver->key_label && driver->key_label(call.key, call.out_text, call.size);
        break;
    }
}

static WindowCall window_call (const WindowCall& call)
{
    WindowCall copy = call;
    os_run_on_window_thread(window_call_now, &copy);
    return copy;
}

const char* os_video_driver ()
{
    return os_state.headless || !os_state.driver ? nullptr : os_state.driver->name;
}

b8 os_set_video_driver (const char* driver)
{
    if (os_state.headless || !driver || !driver[0])
    {
        return false;
    }
    return window_call({ .op = WindowOp::SetVideoDriver, .text = driver }).result;
}

OS_Window* os_create_window (const char* title, u32 width, u32 height, b8 vulkan, u32 x11_visual, b8 hidden)
{
    if (os_state.headless || !os_state.initialized)
    {
        LOG_ERROR("Cannot create a window while headless");
        return nullptr;
    }
    const OS_WindowDesc desc = { .title = title ? title : "puppetxr", .width = width, .height = height, .vulkan = vulkan, .x11_visual = x11_visual, .hidden = hidden };
    return window_call({ .op = WindowOp::Create, .desc = &desc }).window;
}

void os_destroy_window (OS_Window* window)
{
    if (window)
    {
        window_call({ .op = WindowOp::Destroy, .window = window });
    }
}

u32 os_window_id (const OS_Window* window)
{
    return window ? window->id : 0;
}

void* os_window_metal_layer (OS_Window* window)
{
    return window ? window_call({ .op = WindowOp::MetalLayer, .window = window }).pointer : nullptr;
}

b8 os_window_native_handles (OS_Window* window, OS_WindowNative& out_native)
{
    out_native = {};
    if (!window)
    {
        return false;
    }
    return window_call({ .op = WindowOp::NativeHandles, .window = window, .native = &out_native }).result;
}

void os_window_size (OS_Window* window, u32& out_width, u32& out_height)
{
    const WindowCall call = window ? window_call({ .op = WindowOp::Size, .window = window }) : WindowCall {};
    out_width             = call.width_px;
    out_height            = call.height_px;
}

f32 os_window_display_density (OS_Window* window)
{
    const f32 density = window ? window_call({ .op = WindowOp::Density, .window = window }).value : 0.0f;
    return density > 0.0f ? density : 1.0f;
}

f32 os_window_refresh_rate (OS_Window* window)
{
    const f32 rate = window ? window_call({ .op = WindowOp::RefreshRate, .window = window }).value : 0.0f;
    return rate > 0.0f ? rate : 0.0f;
}

b8 os_window_vulkan_extensions (const char* const*& out_names, u32& out_count)
{
    const WindowCall call = window_call({ .op = WindowOp::VulkanExtensions });
    out_names             = call.names;
    out_count             = call.names ? call.count : 0;
    return call.names != nullptr;
}

b8 os_window_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface)
{
    out_surface = 0;
    if (!window || !vk_instance)
    {
        return false;
    }
    const WindowCall call = window_call({ .op = WindowOp::CreateVulkanSurface, .window = window, .vk_instance = vk_instance, .vk_get_instance_proc_addr = vk_get_instance_proc_addr });
    out_surface           = call.surface;
    return call.result;
}

// vkDestroySurfaceKHR of the instance, through the app's loader
void os_window_destroy_vulkan_surface (void* vk_instance, u64 surface)
{
    if (!vk_instance || !surface)
    {
        return;
    }
    typedef void* (*GetInstanceProcAddr)(void* instance, const char* name);
    typedef void  (*DestroySurface)(void* instance, u64 surface, const void* allocator);
    OS_Library*               loader = os_vulkan_loader();
    const GetInstanceProcAddr get    = (GetInstanceProcAddr)os_library_symbol(loader, "vkGetInstanceProcAddr");
    const DestroySurface      destroy = get ? (DestroySurface)get(vk_instance, "vkDestroySurfaceKHR") : nullptr;
    if (destroy)
    {
        destroy(vk_instance, surface, nullptr);
    }
    os_library_close(loader);
}

void os_window_set_relative_mouse (OS_Window* window, b8 relative)
{
    if (window)
    {
        window_call({ .op = WindowOp::RelativeMouse, .window = window, .flag = relative });
    }
}

b8 os_window_maximize (OS_Window* window)
{
    return window && window_call({ .op = WindowOp::Maximize, .window = window }).result;
}

b8 os_window_maximized (OS_Window* window)
{
    return window && window_call({ .op = WindowOp::Maximized, .window = window }).result;
}

static b8 rect_inside (OS_ScreenRect inner, OS_ScreenRect outer)
{
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

void os_window_keep_cursor (OS_Window* window, b8 held)
{
    if (window)
    {
        window_call({ .op = WindowOp::KeepCursor, .window = window, .flag = held });
    }
}

// Held, a clip inside the client area is the relative mode's and stays; any other was set by
// another window and the cursor would leave. Not held, a clip inside the client area is left over
// from holding; another window's goes while this one is in front, as it would otherwise cage the
// cursor in a window the user is not at
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
    const WindowCall call = window ? window_call({ .op = WindowOp::CursorPosition, .window = window }) : WindowCall {};
    out_x                 = call.x;
    out_y                 = call.y;
    return call.result;
}

// The app's own windows, as an editor's play window, open before the first frame shows this one;
// raised, it takes the keyboard and mouse from them. Windows grants that only while the process
// is in the foreground, as an editor is when its play button is clicked.
void os_window_show (OS_Window* window)
{
    if (window)
    {
        window_call({ .op = WindowOp::Show, .window = window });
    }
}

void os_window_hide (OS_Window* window)
{
    if (window)
    {
        window_call({ .op = WindowOp::Hide, .window = window });
    }
}

b8 os_window_shown (OS_Window* window)
{
    return window && window_call({ .op = WindowOp::Shown, .window = window }).result;
}

void os_window_set_cursor (OS_Window* window, OS_Cursor cursor)
{
    if (window && (u32)cursor < (u32)OS_Cursor::Count)
    {
        window_call({ .op = WindowOp::SetCursor, .window = window, .cursor = cursor });
    }
}

void os_window_text_input (OS_Window* window, b8 active, f32 x, f32 y, f32 height)
{
    if (window)
    {
        window_call({ .op = WindowOp::TextInput, .window = window, .x = x, .y = y, .height = height, .flag = active });
    }
}

b8 os_clipboard_text (OS_Window* window, char* out_text, usize size)
{
    if (!out_text || size == 0)
    {
        return false;
    }
    out_text[0] = 0;
    return window && window_call({ .op = WindowOp::ClipboardText, .window = window, .out_text = out_text, .size = size }).result;
}

void os_set_clipboard_text (OS_Window* window, const char* text)
{
    if (window && text)
    {
        window_call({ .op = WindowOp::SetClipboardText, .window = window, .text = text });
    }
}

// Events beyond the queue's capacity are dropped, as os_poll_events drops what does not fit
static void queue_raw_event (const OS_RawEvent& event)
{
    if (!os_state.queue_mutex)
    {
        return;
    }
    os_sys_mutex_lock(os_state.queue_mutex);
    // While the app polls nothing, mouse moves and the repeats of a held key would fill the queue
    // and push out the key and button events after them; past half of it repeats go and a move
    // after a move becomes one, its motion summed. Not before: the simulator ignores a single jump
    // of the cursor, and a merged one hides a real move
    const b8 half_full = os_state.queue_count >= OS_WINDOW_EVENT_QUEUE / 2;
    if (half_full && event.type == OS_RawEventType::KeyDown && event.repeat)
    {
        os_sys_mutex_unlock(os_state.queue_mutex);
        return;
    }
    if (half_full && event.type == OS_RawEventType::MouseMotion)
    {
        OS_RawEvent& last = os_state.queue[(os_state.queue_head + os_state.queue_count - 1) % OS_WINDOW_EVENT_QUEUE];
        if (last.type == OS_RawEventType::MouseMotion && last.window == event.window && last.modifiers == event.modifiers)
        {
            const f32 dx = last.dx + event.dx;
            const f32 dy = last.dy + event.dy;
            last         = event;
            last.dx      = dx;
            last.dy      = dy;
            os_sys_mutex_unlock(os_state.queue_mutex);
            return;
        }
    }
    if (os_state.queue_count < OS_WINDOW_EVENT_QUEUE)
    {
        os_state.queue[(os_state.queue_head + os_state.queue_count) % OS_WINDOW_EVENT_QUEUE] = event;
        os_state.queue_count++;
    }
    os_sys_mutex_unlock(os_state.queue_mutex);
}

void os_queue_event (OS_Window* window, OS_RawEvent event) // NOLINT(misc-const-correctness): callers pass the windows they change
{
    event.window = window ? window->id : event.window;
    queue_raw_event(event);
}

void os_queue_resize (OS_Window* window, u32 width, u32 height, f32 density)
{
    const b8 changed = window->width != width || window->height != height;
    window->width    = width;
    window->height   = height;
    window->density  = density > 0.0f ? density : 1.0f;
    if (changed)
    {
        os_queue_event(window, { .type = OS_RawEventType::WindowResize, .width = width, .height = height });
    }
}

static b8 next_event (OS_RawEvent& out)
{
    if (!os_state.queue_mutex)
    {
        return false;
    }
    os_sys_mutex_lock(os_state.queue_mutex);
    const b8 any = os_state.queue_count > 0;
    if (any)
    {
        out                 = os_state.queue[os_state.queue_head];
        os_state.queue_head = (os_state.queue_head + 1) % OS_WINDOW_EVENT_QUEUE;
        os_state.queue_count--;
    }
    os_sys_mutex_unlock(os_state.queue_mutex);
    return any;
}

b8 os_has_audio_device_ids ()
{
    return HAVE_WINDOWS != 0;
}

// A synthetic event goes straight into the queue for the next os_poll_queued_events
b8 os_push_raw_event (OS_Window* window, const OS_RawEvent& event) // NOLINT(misc-const-correctness): callers pass the windows they change
{
    if (!window || !os_state.initialized)
    {
        return false;
    }
    OS_RawEvent pushed = event;
    pushed.window      = window->id;
    queue_raw_event(pushed);
    return true;
}

static b8 is_mouse_button (OS_Key key)
{
    return key == OS_Key::MouseLeft || key == OS_Key::MouseRight || key == OS_Key::MouseMiddle || key == OS_Key::MouseX1 || key == OS_Key::MouseX2;
}

b8 os_push_mouse_event (OS_Window* window, f32 x, f32 y, OS_Key button, b8 down)
{
    if (!window)
    {
        return false;
    }
    os_state.pushed_window = window->id;
    if (button == OS_Key::None)
    {
        const OS_RawEvent motion = { .type = OS_RawEventType::MouseMotion, .x = x, .y = y, .dx = x - os_state.pushed_x, .dy = y - os_state.pushed_y };
        os_state.pushed_x        = x;
        os_state.pushed_y        = y;
        return os_push_raw_event(window, motion);
    }
    if (!is_mouse_button(button))
    {
        return false;
    }
    // A click puts the cursor where it clicks, so the next move steps from there
    os_state.pushed_x = x;
    os_state.pushed_y = y;
    return os_push_raw_event(window, { .type = down ? OS_RawEventType::MouseButtonDown : OS_RawEventType::MouseButtonUp, .key = button, .x = x, .y = y });
}

b8 os_push_wheel_event (OS_Window* window, f32 dy)
{
    return os_push_raw_event(window, { .type = OS_RawEventType::MouseWheel, .dy = dy });
}

b8 os_push_focus_lost (OS_Window* window)
{
    return os_push_raw_event(window, { .type = OS_RawEventType::FocusLost });
}

b8 os_push_key_event (OS_Window* window, OS_Key key, b8 down, b8 repeat)
{
    if (!window || key == OS_Key::None || (u32)key >= (u32)OS_Key::Count || is_mouse_button(key))
    {
        return false;
    }
    return os_push_raw_event(window, { .type = down ? OS_RawEventType::KeyDown : OS_RawEventType::KeyUp, .key = key, .repeat = repeat });
}

static void pump_now (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    (void)user;
    if (os_state.driver)
    {
        os_state.driver->pump();
    }
}

void os_pump_window_events ()
{
    if (!os_state.initialized || os_state.headless)
    {
        return;
    }
#if HAVE_WINDOW_THREAD
    if (os_state.window_thread)
    {
        os_run_on_window_thread(pump_now, nullptr);
    }
#elif HAVE_MACOS
    if (os_mac_on_main_thread())
    {
        os_mac_serve(0);
        pump_now(nullptr);
    }
#endif
}

u32 os_poll_events (OS_Event* out_events, u32 capacity)
{
    os_pump_window_events();
    return os_poll_queued_events(out_events, capacity);
}

static b8 is_input (OS_RawEventType type)
{
    switch (type)
    {
    case OS_RawEventType::KeyDown:
    case OS_RawEventType::KeyUp:
    case OS_RawEventType::Text:
    case OS_RawEventType::MouseMotion:
    case OS_RawEventType::MouseButtonDown:
    case OS_RawEventType::MouseButtonUp:
    case OS_RawEventType::MouseWheel:
        return true;
    default:
        return false;
    }
}

u32 os_poll_queued_events (OS_Event* out_events, u32 capacity)
{
    ASSERT(out_events || capacity == 0);
    // Events beyond `capacity` wait for the next poll: after a stall, discarding them lost key
    // releases and kept the keys held
    u32         count = 0;
    OS_RawEvent event = {};
    while (count < capacity && next_event(event))
    {
        const b8 button = event.type == OS_RawEventType::MouseButtonDown || event.type == OS_RawEventType::MouseButtonUp;
        if (os_state.raw_hook)
        {
            // Windows may send a window clicks without the moves before them (in Unity's editor,
            // whose input takes the mouse moves), and ImGui clicks where the last move was
            if (button && (event.window != os_state.hook_motion_window || event.x != os_state.hook_motion_x || event.y != os_state.hook_motion_y))
            {
                const OS_RawEvent motion = { .type = OS_RawEventType::MouseMotion, .window = event.window, .modifiers = event.modifiers, .x = event.x, .y = event.y };
                os_state.raw_hook(motion, os_state.raw_hook_user);
            }
            if (event.type == OS_RawEventType::MouseMotion || button)
            {
                os_state.hook_motion_window = event.window;
                os_state.hook_motion_x      = event.x;
                os_state.hook_motion_y      = event.y;
            }
            os_state.raw_hook(event, os_state.raw_hook_user);
        }

        OS_Event translated = {};
        switch (event.type)
        {
        case OS_RawEventType::KeyDown:
        case OS_RawEventType::KeyUp:
            if (event.repeat)
            {
                continue;
            }
            translated.type = event.type == OS_RawEventType::KeyDown ? OS_EventType::KeyDown : OS_EventType::KeyUp;
            translated.key  = event.key;
            break;
        case OS_RawEventType::MouseButtonDown:
        case OS_RawEventType::MouseButtonUp:
            translated.type = event.type == OS_RawEventType::MouseButtonDown ? OS_EventType::KeyDown : OS_EventType::KeyUp;
            translated.key  = event.key;
            translated.x    = event.x;
            translated.y    = event.y;
            break;
        case OS_RawEventType::MouseMotion:
            translated.type = OS_EventType::MouseMotion;
            translated.dx   = event.dx;
            translated.dy   = event.dy;
            break;
        case OS_RawEventType::MouseWheel:
            if (event.dy == 0.0f)
            {
                continue;
            }
            translated.type = OS_EventType::MouseWheel;
            translated.dy   = event.dy;
            break;
        case OS_RawEventType::FocusLost:
            translated.type = OS_EventType::FocusLost;
            break;
        case OS_RawEventType::WindowClose:
            translated.type = OS_EventType::WindowClose;
            break;
        case OS_RawEventType::WindowResize:
            translated.type   = OS_EventType::WindowResize;
            translated.width  = event.width;
            translated.height = event.height;
            break;
        default:
            continue;
        }

        if ((translated.type == OS_EventType::KeyDown || translated.type == OS_EventType::KeyUp) && (translated.key == OS_Key::None || (u32)translated.key >= (u32)OS_Key::Count))
        {
            continue;
        }
        out_events[count++] = translated;
    }
    return count;
}

b8 os_raw_event_is_input (const OS_RawEvent& event)
{
    return is_input(event.type);
}

void os_set_raw_event_hook (OS_RawEventHook hook, void* user)
{
    os_state.raw_hook      = hook;
    os_state.raw_hook_user = user;
}

//
// Offscreen video driver: windows without a window system, which only hold a size
//

static b8 offscreen_init ()
{
    return true;
}

static void offscreen_quit ()
{
}

static b8 offscreen_create_window (OS_Window* window, const OS_WindowDesc& desc)
{
    window->width   = desc.width;
    window->height  = desc.height;
    window->density = 1.0f;
    window->hidden  = desc.hidden;
    return true;
}

static void offscreen_destroy_window (OS_Window* window) // NOLINT(misc-const-correctness): the signature of every driver
{
    (void)window;
}

// Vulkan presents to a headless surface (VK_EXT_headless_surface), which drivers like Mesa's offer
static const char* const* offscreen_vulkan_extensions (u32& out_count)
{
    static const char* const names[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    out_count                        = ARRAY_SIZE(names);
    return names;
}

static b8 offscreen_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface) // NOLINT(misc-const-correctness): the signature of every driver
{
    (void)window;
    struct HeadlessSurfaceCreateInfo
    {
        i32         sType; // VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT
        const void* pNext;
        u32         flags;
    };
    typedef void* (*GetInstanceProcAddr)(void* instance, const char* name);
    typedef i32   (*CreateSurface)(void* instance, const HeadlessSurfaceCreateInfo* info, const void* allocator, u64* surface);
    OS_Library*         loader = vk_get_instance_proc_addr ? nullptr : os_vulkan_loader();
    GetInstanceProcAddr get    = vk_get_instance_proc_addr ? (GetInstanceProcAddr)(uintptr_t)vk_get_instance_proc_addr : (GetInstanceProcAddr)os_library_symbol(loader, "vkGetInstanceProcAddr");
    const CreateSurface create = get ? (CreateSurface)get(vk_instance, "vkCreateHeadlessSurfaceEXT") : nullptr;
    const HeadlessSurfaceCreateInfo info = { .sType = 1000256000, .pNext = nullptr, .flags = 0 };
    const b8            created = create && create(vk_instance, &info, nullptr, &out_surface) == 0;
    os_library_close(loader);
    if (!created)
    {
        LOG_ERROR("vkCreateHeadlessSurfaceEXT failed or is missing");
    }
    return created;
}

static void offscreen_pump ()
{
}

static void offscreen_wait (u64 timeout_ns)
{
#if HAVE_WINDOW_THREAD
    os_sys_wake_wait(timeout_ns);
#else
    os_sleep_ns(timeout_ns);
#endif
}

const OS_VideoDriver os_offscreen_driver = {
    .name           = "offscreen",
    .init           = offscreen_init,
    .quit           = offscreen_quit,
    .create_window  = offscreen_create_window,
    .destroy_window = offscreen_destroy_window,
    .vulkan_extensions     = offscreen_vulkan_extensions,
    .create_vulkan_surface = offscreen_create_vulkan_surface,
    .pump           = offscreen_pump,
    .wait           = offscreen_wait,
};

//
// Mutex
//

// A mutex is held for short stretches, so a waiter spins a little first. On macOS the waiting
// main thread serves window calls, as the holder may wait for one; so does the waiting window
// thread elsewhere: a frame's window tick waits for the runtime lock, which xrDestroySession holds
// while the panels shut down there
void os_mutex_lock (OS_Mutex* mutex)
{
    const u64 start = os_monotonic_ns();
    do
    {
        if (os_sys_mutex_try_lock(mutex))
        {
            return;
        }
    } while (os_monotonic_ns() - start < OS_MUTEX_SPIN_NS);
#if HAVE_MACOS
    if (os_mac_on_main_thread())
    {
        os_mac_lock_on_main(mutex);
        return;
    }
#endif
#if HAVE_WINDOW_THREAD
    if (on_window_thread())
    {
        os_atomic_add(&os_state.window_waiting, 1);
        for (;;)
        {
            run_window_thread_call(0);
            if (os_sys_mutex_try_lock(mutex))
            {
                break;
            }
            os_sys_semaphore_wait_ns(os_state.window_wake, 5000000ull);
        }
        os_atomic_add(&os_state.window_waiting, -1);
        return;
    }
#endif
    os_sys_mutex_lock(mutex);
}

void os_mutex_unlock (OS_Mutex* mutex)
{
    os_sys_mutex_unlock(mutex);
#if HAVE_MACOS
    os_mac_unlocked();
#endif
#if HAVE_WINDOW_THREAD
    if (os_atomic_load(&os_state.window_waiting) > 0)
    {
        os_semaphore_signal(os_state.window_wake);
    }
#endif
}

//
// Atomics
//

#if defined(_MSC_VER) && !defined(__clang__)
static_assert(sizeof(long) == sizeof(i32));

i32 os_atomic_load (const OS_Atomic32* atomic)
{
    return (i32)_InterlockedCompareExchange((volatile long*)&atomic->value, 0, 0);
}

void os_atomic_store (OS_Atomic32* atomic, i32 value)
{
    _InterlockedExchange((volatile long*)&atomic->value, value);
}

i32 os_atomic_add (OS_Atomic32* atomic, i32 value)
{
    return (i32)_InterlockedExchangeAdd((volatile long*)&atomic->value, value);
}

b8 os_atomic_compare_swap (OS_Atomic32* atomic, i32 expected, i32 desired)
{
    return _InterlockedCompareExchange((volatile long*)&atomic->value, desired, expected) == expected;
}
#else
i32 os_atomic_load (const OS_Atomic32* atomic)
{
    return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}

void os_atomic_store (OS_Atomic32* atomic, i32 value)
{
    __atomic_store_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

i32 os_atomic_add (OS_Atomic32* atomic, i32 value)
{
    return __atomic_fetch_add(&atomic->value, value, __ATOMIC_SEQ_CST);
}

b8 os_atomic_compare_swap (OS_Atomic32* atomic, i32 expected, i32 desired)
{
    return __atomic_compare_exchange_n(&atomic->value, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#endif

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
    OS_Atomic32   count;
    u32           capture_start;
    u64           thread_id;
    char          thread_name[64];
    ProfileOpen   open[OS_PROFILE_MAX_DEPTH];
    u32           depth;
};

struct ProfileState
{
    OS_Atomic32   active;
    OS_Atomic32   generation;
    OS_Atomic32   lock;
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

static void profile_lock ()
{
    while (!os_atomic_compare_swap(&os_profile.lock, 0, 1))
    {
    }
}

static void profile_unlock ()
{
    os_atomic_store(&os_profile.lock, 0);
}

static void profile_thread_name (ProfileRing& ring)
{
    os_sys_thread_name(ring.thread_name, sizeof(ring.thread_name));
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
    profile_lock();
    if (os_profile.rings_count < OS_PROFILE_MAX_THREADS)
    {
        ProfileRing* ring = (ProfileRing*)os_memory_alloc(sizeof(ProfileRing));
        ring->thread_id   = os_thread_current_id();
        profile_thread_name(*ring);
        ring->capture_start = 0;
        os_profile.rings[os_profile.rings_count++] = ring;
        os_profile_ring = ring;
    }
    profile_unlock();
    return os_profile_ring;
}

b8 os_profile_active ()
{
    return os_atomic_load(&os_profile.active) != 0;
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
        ring->open[depth] = { .name = name, .start_ns = os_monotonic_ns(), .generation = os_atomic_load(&os_profile.generation) };
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
    if (!os_profile_active() || open.generation != os_atomic_load(&os_profile.generation))
    {
        return;
    }
    const u32 count = (u32)os_atomic_load(&ring->count);
    ring->events[count % OS_PROFILE_RING_EVENTS] = { .name = open.name, .start_ns = open.start_ns, .end_ns = os_monotonic_ns() };
    os_atomic_store(&ring->count, (i32)(count + 1));
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
    profile_lock();
    os_profile.file     = file;
    os_profile.start_ns = os_monotonic_ns();
    for (u32 i = 0; i < os_profile.rings_count; i++)
    {
        os_profile.rings[i]->capture_start = (u32)os_atomic_load(&os_profile.rings[i]->count);
    }
    os_atomic_add(&os_profile.generation, 1);
    os_atomic_store(&os_profile.active, 1);
    profile_unlock();
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
    profile_lock();
    os_atomic_store(&os_profile.active, 0);
    const ProfileRing* rings[OS_PROFILE_MAX_THREADS] = {};
    const u32          rings_count                   = os_profile.rings_count;
    for (u32 i = 0; i < rings_count; i++)
    {
        rings[i] = os_profile.rings[i];
    }
    OS_File* file       = os_profile.file;
    os_profile.file     = nullptr;
    const u64 start_ns  = os_profile.start_ns;
    profile_unlock();

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
        const ProfileRing& ring  = *rings[r];
        const u32          end   = (u32)os_atomic_load(&ring.count);
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
        os_state.log_mutex = os_mutex_create();
        os_state.start_ns  = os_monotonic_ns();
    }
    os_sys_mutex_lock(os_state.log_mutex);
    os_state.log_callback      = callback;
    os_state.log_callback_user = user;
    os_sys_mutex_unlock(os_state.log_mutex);
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

    log_append(level, OS_LogSource::Sim, text);

    // While capturing, stderr is our own pipe; write straight to the terminal instead
    if (os_state.capture_active)
    {
        os_sys_fd_write(os_state.saved_stderr, "puppetxr: ", 7);
        os_sys_fd_write(os_state.saved_stderr, text, cstr_length(text));
        os_sys_fd_write(os_state.saved_stderr, "\n", 1);
        return;
    }
    os_sys_log_write(level, text);
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
    if (!os_sys_pipe(fds))
    {
        return false;
    }
    (void)fflush(stdout);
    (void)fflush(stderr);
    os_state.saved_stdout = os_sys_fd_dup(1);
    os_state.saved_stderr = os_sys_fd_dup(2);
    os_sys_fd_dup2(fds[1], 1);
    os_sys_fd_dup2(fds[1], 2);
    os_sys_fd_close(fds[1]);
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
        const i64 count = os_sys_fd_read_available(os_state.capture_read, buffer, sizeof(buffer));
        if (count <= 0)
        {
            break;
        }
        os_sys_fd_write(os_state.saved_stdout, buffer, (usize)count);
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
        os_sys_fd_dup2(os_state.saved_stdout, 1);
        os_sys_fd_close(os_state.saved_stdout);
    }
    if (os_state.saved_stderr >= 0)
    {
        os_sys_fd_dup2(os_state.saved_stderr, 2);
        os_sys_fd_close(os_state.saved_stderr);
    }
    os_sys_fd_close(os_state.capture_read);
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

//
// Keys
//

const char* os_key_name (OS_Key key)
{
    return (u32)key < (u32)OS_Key::Count ? os_key_names[(u32)key] : "none";
}

namespace
{

struct KeyLabelCall
{
    OS_Key key;
    char*  out;
    usize  size;
    b8     result;
};

} // namespace

// Keys are physical positions named after US QWERTY; the layout names them, as the key left of
// 1 that is ^ on German layouts
void os_key_label (OS_Key key, char* out, usize size)
{
    ASSERT(out && size > 0);
    out[0] = 0;
    if (os_state.initialized && !os_state.headless && os_state.driver && os_state.driver->key_label && (u32)key < (u32)OS_Key::Count)
    {
        const WindowCall call = window_call({ .op = WindowOp::KeyLabel, .out_text = out, .size = size, .key = key });
        if (call.result && out[0])
        {
            return;
        }
    }
    const char* label = os_key_ascii_label(key);
    cstr_copy(out, size, label[0] ? label : os_key_name(key));
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
    return memmove(dest, source, size);
}

void* memory_copy (void* dest, const void* source, usize size)
{
    if (size == 0)
    {
        return dest;
    }
    return memcpy(dest, source, size);
}

void* memory_zero (void* dest, usize size)
{
    if (size == 0)
    {
        return dest;
    }
    return memset(dest, 0, size);
}

b8 memory_equal (const void* a, const void* b, usize size)
{
    if (size == 0)
    {
        return true;
    }
    return memcmp(a, b, size) == 0;
}

b8 cstr_equal (const char* a, const char* b)
{
    if (!a || !b)
    {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

usize cstr_length (const char* str)
{
    return str ? strlen(str) : 0;
}

b8 cstr_copy (char* dest, usize dest_size, const char* source)
{
    ASSERT(dest && dest_size > 0);
    if (!source)
    {
        dest[0] = 0;
        return true;
    }
    // Callers pass a field as its own default
    if (dest == source)
    {
        return strlen(dest) < dest_size;
    }
    const usize length = strlen(source);
    const usize copied = length < dest_size ? length : dest_size - 1;
    if (copied > 0)
    {
        // NOLINTNEXTLINE(clang-analyzer-unix.cstring.UninitializedRead): copied is at most strlen(source)
        memmove(dest, source, copied);
    }
    dest[copied] = 0;
    return length < dest_size;
}

b8 cstr_starts_with (const char* str, const char* prefix)
{
    if (!str || !prefix)
    {
        return false;
    }
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

const char* cstr_find (const char* str, const char* needle)
{
    if (!str || !needle)
    {
        return nullptr;
    }
    return strstr(str, needle);
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
    // vsnprintf leaves the buffer as it was when the format fails
    if (dest && dest_size > 0)
    {
        dest[0] = 0;
    }
    const i32 length = vsnprintf(dest, dest_size, fmt, args);
    return length < 0 ? 0 : (usize)length;
}

static void swap_bytes (u8* a, u8* b, usize size)
{
    for (usize i = 0; i < size; i++)
    {
        const u8 t = a[i];
        a[i]       = b[i];
        b[i]       = t;
    }
}

namespace
{

struct RawCompare
{
    i32 (*compare)(const void*, const void*);
};

} // namespace

static i32 raw_compare (const void* a, const void* b, const void* context)
{
    return ((const RawCompare*)context)->compare(a, b);
}

static void sift_down (u8* data, usize root, usize count, usize size, i32 (*compare)(const void*, const void*, const void*), const void* context)
{
    for (;;)
    {
        usize child = root * 2 + 1;
        if (child >= count)
        {
            return;
        }
        if (child + 1 < count && compare(data + child * size, data + (child + 1) * size, context) < 0)
        {
            child++;
        }
        if (compare(data + root * size, data + child * size, context) >= 0)
        {
            return;
        }
        swap_bytes(data + root * size, data + child * size, size);
        root = child;
    }
}

// The same algorithm on every platform, so equal elements end in the same order everywhere (the C
// libraries' qsort differ): insertion sort for short ranges, heapsort otherwise
void quicksort_context (void* data, usize count, usize element_size, i32 (*compare)(const void*, const void*, const void* context), const void* context)
{
    u8* const bytes = (u8*)data;
    if (count < 2)
    {
        return;
    }
    if (count <= 16)
    {
        for (usize i = 1; i < count; i++)
        {
            for (usize k = i; k > 0 && compare(bytes + (k - 1) * element_size, bytes + k * element_size, context) > 0; k--)
            {
                swap_bytes(bytes + (k - 1) * element_size, bytes + k * element_size, element_size);
            }
        }
        return;
    }
    for (usize root = count / 2; root-- > 0;)
    {
        sift_down(bytes, root, count, element_size, compare, context);
    }
    for (usize end = count - 1; end > 0; end--)
    {
        swap_bytes(bytes, bytes + end * element_size, element_size);
        sift_down(bytes, 0, end, element_size, compare, context);
    }
}

void quicksort_raw (void* data, usize count, usize element_size, i32 (*compare)(const void*, const void*))
{
    const RawCompare raw = { compare };
    quicksort_context(data, count, element_size, raw_compare, &raw);
}
