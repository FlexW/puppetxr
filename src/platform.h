#pragma once

#include <inttypes.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float  f32;
typedef double f64;

typedef size_t usize;

typedef bool b8;

// Formatting goes through SDL_vsnprintf, which takes the C99 conversions on every platform;
// mingw's plain printf format means the Microsoft runtime's
#if defined(__MINGW32__)
#define PRINTF_LIKE(fmt_idx, first_arg) __attribute__((format(gnu_printf, fmt_idx, first_arg)))
#elif defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define PRINTF_LIKE(fmt_idx, first_arg)
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define UNUSED(x)       (void)(x)

#if defined(__FILE_NAME__)
#define FILE_NAME __FILE_NAME__
#else
#define FILE_NAME __FILE__
#endif

#define FUNCTION_NAME __func__
#define LINE_NUMBER   __LINE__

#if defined(_WIN32) || defined(_WIN64)
#define HAVE_WINDOWS 1
#else
#define HAVE_WINDOWS 0
#endif

#if defined(__APPLE__)
#define HAVE_MACOS 1
#else
#define HAVE_MACOS 0
#endif

// size_t is unsigned long on macOS and uint64_t unsigned long long, two distinct types
#define USIZE_IS_U64 (!HAVE_MACOS)

#if defined(__linux__)
#define HAVE_LINUX 1
#else
#define HAVE_LINUX 0
#endif

#if defined(DEBUG) || defined(_DEBUG) || !defined(NDEBUG)
#define HAVE_DEBUG 1
#else
#define HAVE_DEBUG 0
#endif

#if !defined(PUPPETXR_VALIDATION)
#define PUPPETXR_VALIDATION 1
#endif

#if !defined(PUPPETXR_DIST)
#define PUPPETXR_DIST 0
#endif

#if !defined(PUPPETXR_HAVE_D3D)
#define PUPPETXR_HAVE_D3D 0
#endif

#if !defined(PUPPETXR_HAVE_GL)
#define PUPPETXR_HAVE_GL 1
#endif

#if !defined(PUPPETXR_HAVE_METAL)
#define PUPPETXR_HAVE_METAL 0
#endif

#if !defined(PUPPETXR_BUILD_VERSION_MAJOR)
#define PUPPETXR_BUILD_VERSION_MAJOR 0
#define PUPPETXR_BUILD_VERSION_MINOR 0
#define PUPPETXR_BUILD_VERSION_PATCH 0
#endif

// Same packing as PUPPETXR_MAKE_VERSION in the public puppetxr.h; tests_api.cpp checks they agree
#define PUPPETXR_VERSION ((u32)(PUPPETXR_BUILD_VERSION_MAJOR) << 22 | (u32)(PUPPETXR_BUILD_VERSION_MINOR) << 12 | (u32)(PUPPETXR_BUILD_VERSION_PATCH))

#if HAVE_WINDOWS
#define PUPPETXR_EXPORT extern "C" __declspec(dllexport)
#else
#define PUPPETXR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define OS_MAX_PATH        1024
#define OS_LOG_HISTORY     512
#define OS_LOG_LINE_LENGTH 240

enum class OS_LogLevel
{
    Debug,
    Info,
    Warning,
    Error,
};

enum class OS_AssertMode
{
    Break,
    Log,
};

// Keys and mouse buttons the simulator can bind. Names for config files come from os_key_name.
enum class OS_Key : u8
{
    None = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Space, Escape, Enter, Tab, Backspace,
    ArrowLeft, ArrowRight, ArrowUp, ArrowDown,
    LeftShift, LeftCtrl, LeftAlt, RightShift, RightCtrl, RightAlt,
    MouseLeft, MouseRight, MouseMiddle,
    Grave,        // left of 1: ` on US layouts, ^ on German, ² on French
    LeftBracket,  // right of P: [ on US layouts, Ü on German
    RightBracket, // ] on US layouts, + on German
    Count,
};

struct OS_Mutex;
struct OS_Semaphore;
struct OS_Thread;
struct OS_Window;
struct OS_File;
struct OS_Socket;
struct OS_LockFile;
struct OS_Library;
struct OS_Process;

enum class OS_SocketResult : u8
{
    Ok = 0,
    WouldBlock,
    Closed, // the peer closed the connection or it failed
};

enum class OS_LogSource : u8
{
    Sim = 0, // our LOG_* calls
    App,     // the host process's stdout and stderr, when captured
};

// What a graphics API presents to: the window of the video driver
struct OS_WindowNative
{
    u64   window;  // X11 Window, wl_surface* or HWND
    void* display; // Display*, wl_display* or the window's own HDC
    b8    wayland;
};

// The clock apps hand to the time conversion extensions
enum class OS_NativeClock : u8
{
    Timespec = 0,       // CLOCK_MONOTONIC as a struct timespec, XR_KHR_convert_timespec_time
    PerformanceCounter, // QueryPerformanceCounter ticks, XR_KHR_win32_convert_performance_counter_time
};

struct OS_LogLine
{
    OS_LogLevel  level;
    OS_LogSource source;
    u64          time_ns;
    char         text[OS_LOG_LINE_LENGTH];
};

enum class OS_EventType : u8
{
    None = 0,
    KeyDown,
    KeyUp,
    MouseMotion,
    MouseWheel,
    WindowClose,
    WindowResize,
    FocusLost,
};

struct OS_Event
{
    OS_EventType type;
    OS_Key       key;    // KeyDown, KeyUp (mouse buttons arrive as keys)
    f32          dx;     // MouseMotion
    f32          dy;     // MouseMotion, MouseWheel (positive away from the user)
    f32          x;      // KeyDown, KeyUp of a mouse button: where in window pixels
    f32          y;
    u32          width;  // WindowResize
    u32          height;
};

// Screen pixels, right and bottom exclusive
struct OS_ScreenRect
{
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
};

// What a window does with the system's cursor clip, which other windows of the process set too
// (Unreal clips the cursor to its game window while that is in front)
enum class OS_ClipAction : u8
{
    Keep = 0,
    Clip, // to the window's client area
    Free,
};

// Raw platform events are forwarded here before translation, for the UI layer
typedef void (*OS_RawEventHook)(const void* native_event, b8 input, void* user); // input: a key, mouse or text event

b8            os_initialize         (b8 headless);
void          os_shutdown           ();
void          os_release            (); // frees the log mutex and closes the log file, for unloading; logging recreates them
b8            os_is_headless        ();

void*         os_memory_alloc       (usize size);
void*         os_memory_realloc     (void* pointer, usize size);
void          os_memory_release     (void* pointer);

u64           os_time_ns            ();
u64           os_monotonic_ns       (); // the native clock in ns, the one apps hand to the time conversion extensions
u64           os_timespec_ns        (); // the clock of XR_KHR_convert_timespec_time: CLOCK_MONOTONIC, on Windows the UTC wall clock apps read with timespec_get
OS_NativeClock os_native_clock      ();
u64           os_native_ticks_to_ns (i64 ticks); // native clock ticks (ns for a timespec) to ns, 0 for ticks <= 0
i64           os_ns_to_native_ticks (u64 ns);
void          os_sleep_ns           (u64 ns);

const char*   os_env                (const char* name);
void          os_set_env            (const char* name, const char* value); // the C runtime's environment; not thread safe, set before threads start
void          os_unset_env          (const char* name);

b8            os_file_exists        (const char* path);
b8            os_directory_exists   (const char* path);
b8            os_module_directory   (char* out_dir, usize out_size); // directory of the library or executable this code is in
OS_Library*   os_library_open       (const char* name); // a shared library by file name, loaded if needed; null when there is none
b8            os_library_path       (const char* name, char* out_path, usize size); // where a loaded library by file or install name lives; macOS only
OS_Library*   os_vulkan_loader      (); // the Vulkan loader the app already loaded, or null
void          os_library_close      (OS_Library* library);
void*         os_library_symbol     (OS_Library* library, const char* name); // a null library searches everything the process loaded
b8            os_module_loaded      (const char* name); // a shared library by file name is in the process; never loads it
b8            os_read_file          (const char* path, u8** out_data, usize* out_size);
b8            os_write_file         (const char* path, const void* data, usize size);
b8            os_write_file_atomic  (const char* path, const void* data, usize size); // readers see the old or the new file, never a part
OS_File*      os_file_create        (const char* path); // truncates an existing file
OS_File*      os_file_append        (const char* path); // writes at the end, created when missing; others may read and append meanwhile
b8            os_file_write         (OS_File* file, const void* data, usize size);
b8            os_file_flush         (OS_File* file);
b8            os_file_close         (OS_File* file);    // false if the close or any earlier write failed
b8            os_list_directory     (const char* path, void (*callback)(const char* name, void* user), void* user); // entry names in no particular order

b8            os_delete_file        (const char* path);
b8            os_delete_tree        (const char* path); // a directory with everything in it; true if it is gone afterwards
b8            os_create_directory   (const char* path); // true if it exists afterwards
b8            os_create_directories (const char* path); // with every missing parent
b8            os_runtime_dir        (char* out_path, usize size); // per-user directory for sockets and locks
b8            os_state_dir          (char* out_path, usize size); // per-user persistent directory of puppetxr, created on demand; $PUPPETXR_STATE_DIR overrides
b8            os_current_directory  (char* out_path, usize size); // with a trailing separator
b8            os_path_is_absolute   (const char* path);
b8            os_temp_dir           (char* out_path, usize size); // $TMPDIR or /tmp, the user's temporary directory on Windows; no trailing separator
b8            os_create_temp_directory (const char* prefix, char* out_path, usize size); // a new empty directory in os_temp_dir
u32           os_process_id         ();
b8            os_process_alive      (u32 pid); // a process of that id runs; false for 0

// A child process with argv[0] as the program and a null-terminated argv. Without paths it
// shares the standard streams; stdout_path takes stderr too. The environment is the process's
// at the time of the call.
OS_Process*   os_process_spawn      (const char* const* argv, const char* stdin_path, const char* stdout_path);
b8            os_process_wait       (OS_Process* process, u32 timeout_ms, i32& out_exit_code); // false while it still runs
void          os_process_release    (OS_Process* process); // kills it if it still runs

// TCP sockets, all non-blocking and closed on exec. Sending to a closed peer reports Closed
// instead of raising SIGPIPE.
OS_Socket*    os_socket_listen      (const char* address, u16 port); // port 0 lets the system pick one
u16           os_socket_port        (const OS_Socket* socket);
OS_Socket*    os_socket_accept      (const OS_Socket* listener);           // null when nobody is waiting
OS_Socket*    os_socket_connect     (const char* address, u16 port, u32 timeout_ms);
OS_SocketResult os_socket_send      (const OS_Socket* socket, const void* data, usize size, usize& out_sent);
OS_SocketResult os_socket_receive   (const OS_Socket* socket, void* data, usize capacity, usize& out_received);
b8            os_socket_wait        (const OS_Socket* socket, u32 timeout_ms, b8 for_writing = false); // readable (or closed), or writable
void          os_socket_close       (OS_Socket* socket);

// A file that holds an exclusive lock while its process lives. The kernel drops the lock however
// the process ends, so a file without a holder is stale, even after a crash.
OS_LockFile*  os_lock_file_create   (const char* path, const void* data, usize size);
void          os_lock_file_release  (OS_LockFile* file); // deletes the file
b8            os_lock_file_is_stale (const char* path);  // the file exists and no process holds it

const char*   os_video_driver       (); // "x11", "wayland", "offscreen", ...; null when headless
b8            os_set_video_driver   (const char* driver); // restarts video on that driver; destroy every window first. A failed switch keeps the old one
OS_Window*    os_create_window      (const char* title, u32 width, u32 height, b8 vulkan, u32 x11_visual = 0, b8 hidden = false); // x11_visual: the X visual id the window must have, for GLX
b8            os_window_native_handles (OS_Window* window, OS_WindowNative& out_native); // false for drivers without a native window
void          os_destroy_window     (OS_Window* window);
void*         os_window_native      (OS_Window* window); // SDL_Window*
void*         os_window_metal_layer (OS_Window* window); // the CAMetalLayer of a view made once per window; null where there is no Metal
void          os_window_size        (OS_Window* window, u32& out_width, u32& out_height); // in pixels
f32           os_window_display_density (OS_Window* window); // pixels per point of the window's display, 1 when unknown
f32           os_window_refresh_rate (OS_Window* window); // refresh rate in Hz of the window's display, 0 when unknown
b8            os_window_vulkan_extensions (const char* const*& out_names, u32& out_count);
b8            os_window_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface);
void          os_window_destroy_vulkan_surface (void* vk_instance, u64 surface);
void          os_window_set_relative_mouse (OS_Window* window, b8 relative);
b8            os_window_maximize    (OS_Window* window); // false where the video driver cannot, as offscreen; the window manager may apply it a few events later
b8            os_window_maximized   (OS_Window* window);
void          os_window_show        (OS_Window* window); // a window created or made hidden, raised in front of the process's other windows
void          os_window_hide        (OS_Window* window);
b8            os_window_cursor      (OS_Window* window, f32& out_x, f32& out_y); // the system cursor in window pixels while it is over the window, not held relative and the window got no pushed mouse events (Windows); false elsewhere
void          os_window_keep_cursor (OS_Window* window, b8 held); // once a frame: held, the cursor stays in the window; not held, it is free while the window is in front (Windows; nothing elsewhere)
OS_ClipAction os_cursor_clip_action (b8 held, b8 foreground, OS_ScreenRect clip, OS_ScreenRect client, OS_ScreenRect desktop);
b8            os_window_shown       (OS_Window* window);
u32           os_poll_events        (OS_Event* out_events, u32 capacity); // returns the number written, drops the rest
void          os_set_raw_event_hook (OS_RawEventHook hook, void* user); // called from os_poll_events
b8            os_has_audio_device_ids (); // the platform names audio endpoints (Windows)
b8            os_default_audio_device (b8 input, wchar_t* out_id, usize count); // the default endpoint's id, empty and false without one
b8            os_push_mouse_event   (OS_Window* window, f32 x, f32 y, OS_Key button, b8 down); // synthetic input at window pixel (x, y) for the next os_poll_events: a move for OS_Key::None, else the button there
b8            os_push_key_event     (OS_Window* window, OS_Key key, b8 down, b8 repeat); // synthetic key for the next os_poll_events; false for keys without a scancode
b8            os_push_wheel_event   (OS_Window* window, f32 dy);                       // synthetic wheel turn for the next os_poll_events, positive away from the user
b8            os_push_focus_lost    (OS_Window* window);                               // synthetic loss of the keyboard focus for the next os_poll_events

// Runs `function` on the thread that owns the windows and waits for it. Where windows belong to
// no thread (X11, Wayland) it runs on the calling thread. Code that calls SDL's window or input
// functions itself, as the ImGui backend does, goes through here; os_* window functions do it
// on their own. The function must not wait for the caller.
// On macOS the window thread is the app's main thread; a call it does not start within
// OS_MAIN_THREAD_TIMEOUT_MS is withdrawn without running, and later calls fail at once until the
// main thread serves again (it does whenever it runs its run loop, waits for an os mutex or polls).
#define OS_MAIN_THREAD_TIMEOUT_MS 250
#define OS_MUTEX_SPIN_NS          200000
typedef void  OS_WindowThreadFunction (void* user);
void          os_run_on_window_thread (OS_WindowThreadFunction* function, void* user);
void          os_post_to_window_thread (OS_WindowThreadFunction* function, void* user); // runs it later on the window thread without waiting, at once when called there; `user` must stay valid until it ran
b8            os_is_window_thread   (); // puppetxr's window thread on Windows, the main thread on macOS; any thread elsewhere and when headless
void          os_serve_posted       (u64 ns); // window thread: runs what was posted, and on macOS the run loop for `ns`; elsewhere waits `ns`. For tests, which run no event loop
void          os_pump_window_events (); // window thread: lets the window system deliver its events into the queue, unless the app's running event loop does
u32           os_poll_queued_events (OS_Event* out_events, u32 capacity); // os_poll_events without pumping: what is queued already, from any thread

OS_Mutex*     os_mutex_create       ();
void          os_mutex_destroy      (OS_Mutex* mutex);
void          os_mutex_lock         (OS_Mutex* mutex);
void          os_mutex_unlock       (OS_Mutex* mutex);
OS_Semaphore* os_semaphore_create   (u32 initial);
void          os_semaphore_destroy  (OS_Semaphore* semaphore);
void          os_semaphore_wait     (OS_Semaphore* semaphore);
b8            os_semaphore_try_wait (OS_Semaphore* semaphore);
void          os_semaphore_signal   (OS_Semaphore* semaphore);
typedef i32   OS_ThreadFunction     (void* user);
OS_Thread*    os_thread_create      (const char* name, OS_ThreadFunction* function, void* user);
i32           os_thread_join        (OS_Thread* thread); // returns the function's result
u64           os_thread_current_id  (); // the calling thread's id, never 0

// Zone profiler (docs/profiling.md): a capture writes a Chrome trace of every zone that began and
// ended while it ran, and logs the zones of the most total time. Stopped, a zone costs one atomic
// load, so zones stay compiled in, dist builds too. Names are string literals or __func__
#define OS_PROFILE_RING_EVENTS  65536 // per thread; a thread keeps the latest
#define OS_PROFILE_MAX_THREADS  64    // threads that ever record; later ones are not recorded
#define OS_PROFILE_MAX_DEPTH    32    // deeper zones are not recorded
b8            os_profile_start      (const char* path); // false while one runs or the file cannot be made
void          os_profile_stop       (); // writes the trace and the summary
b8            os_profile_active     ();
void          os_profile_begin      (const char* name);
void          os_profile_end        ();

// Lasts to the end of its scope; a zone begun while stopped is never recorded
struct OS_ProfileZone
{
    b8 recording = false;

    explicit OS_ProfileZone (const char* name) : recording(os_profile_active())
    {
        if (recording)
        {
            os_profile_begin(name);
        }
    }

    ~OS_ProfileZone ()
    {
        if (recording)
        {
            os_profile_end();
        }
    }

    OS_ProfileZone (const OS_ProfileZone&)            = delete;
    OS_ProfileZone& operator= (const OS_ProfileZone&) = delete;
};

#define PROF_CONCAT_(a, b) a##b
#define PROF_CONCAT(a, b)  PROF_CONCAT_(a, b)
#define PROF_ZONE(name)    const OS_ProfileZone PROF_CONCAT(prof_zone_, __LINE__)(name)

void          os_log                (OS_LogLevel level, const char* fmt, ...) PRINTF_LIKE(2, 3);
void          os_log_v              (OS_LogLevel level, const char* fmt, va_list args) PRINTF_LIKE(2, 0);
void          os_log_text           (OS_LogLevel level, const char* text); // a line of any length: the log file, stderr and the callback get it whole, the history its first OS_LOG_LINE_LENGTH bytes
void          os_set_log_level      (OS_LogLevel level);
typedef void  OS_LogCallback        (OS_LogLevel level, const char* text, void* user);
void          os_set_log_callback   (OS_LogCallback* callback, void* user); // runtime lines that pass the level, on the logging thread; null removes
u32           os_log_history_count  ();                 // lines kept, at most OS_LOG_HISTORY
const OS_LogLine* os_log_history    (u32 index);        // 0 is the oldest kept line
u64           os_log_sequence       ();                 // total lines ever logged, for change detection
b8            os_claim_once         (u32* flag);        // true for the first caller only, on any thread; flag starts at 0

// Routes the process's stdout and stderr through the runtime: lines land in the log history as
// OS_LogSource::App and are still written to the original streams. Poll once per frame.
b8            os_capture_output_begin ();
void          os_capture_output_poll  ();
void          os_capture_output_end   ();

void          os_set_assert_mode    (OS_AssertMode mode);
OS_AssertMode os_default_assert_mode (); // Log in dist builds, Break otherwise
u32           os_assert_count       ();
b8            os_report_assert      (const char* expression, const char* function, const char* file, i32 line, const char* message = nullptr, ...) PRINTF_LIKE(5, 6);
b8            os_report_failure     (const char* function, const char* file, i32 line, const char* message, ...) PRINTF_LIKE(4, 5);
void          os_trigger_breakpoint ();

const char*   os_key_name           (OS_Key key);
b8            os_key_from_name      (const char* name, OS_Key& out_key);
void          os_key_label          (OS_Key key, char* out, usize size); // as printed on the keyboard layout in use, e.g. "Z" for y on QWERTZ

void*         memory_move           (void* dest, const void* source, usize size);
void*         memory_copy           (void* dest, const void* source, usize size);
void*         memory_zero           (void* dest, usize size);
b8            memory_equal          (const void* a, const void* b, usize size);

b8            cstr_equal            (const char* a, const char* b);
usize         cstr_length           (const char* str);
b8            cstr_copy             (char* dest, usize dest_size, const char* source);
b8            cstr_starts_with      (const char* str, const char* prefix);
const char*   cstr_find             (const char* str, const char* needle);
usize         cstr_format           (char* dest, usize dest_size, const char* fmt, ...) PRINTF_LIKE(3, 4);
usize         cstr_format_v         (char* dest, usize dest_size, const char* fmt, va_list args) PRINTF_LIKE(3, 0);

void          quicksort_raw         (void* data, usize count, usize element_size, i32 (*compare)(const void*, const void*));

#define LOG_DEBUG(fmt, ...)   os_log(OS_LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    os_log(OS_LogLevel::Info, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) os_log(OS_LogLevel::Warning, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   os_log(OS_LogLevel::Error, fmt, ##__VA_ARGS__)

#if PUPPETXR_VALIDATION

#define ASSERT(condition)                                                                       \
    do                                                                                          \
    {                                                                                           \
        if (!(condition))                                                                       \
        {                                                                                       \
            if (os_report_assert(#condition, FUNCTION_NAME, FILE_NAME, LINE_NUMBER))            \
            {                                                                                   \
                os_trigger_breakpoint();                                                        \
            }                                                                                   \
        }                                                                                       \
    }                                                                                           \
    while (0)

#define ASSERT_MSG(condition, message, ...)                                                                     \
    do                                                                                                          \
    {                                                                                                           \
        if (!(condition))                                                                                       \
        {                                                                                                       \
            if (os_report_assert(#condition, FUNCTION_NAME, FILE_NAME, LINE_NUMBER, message, ##__VA_ARGS__))    \
            {                                                                                                   \
                os_trigger_breakpoint();                                                                        \
            }                                                                                                   \
        }                                                                                                       \
    }                                                                                                           \
    while (0)

#define FAIL_MSG(message, ...)                                                                  \
    do                                                                                      \
    {                                                                                       \
        if (os_report_failure(FUNCTION_NAME, FILE_NAME, LINE_NUMBER, message, ##__VA_ARGS__)) \
        {                                                                                   \
            os_trigger_breakpoint();                                                        \
        }                                                                                   \
    }                                                                                       \
    while (0)

#else

#define ASSERT(condition)                   do { } while (0)
#define ASSERT_MSG(condition, message, ...) do { } while (0)
#define FAIL_MSG(message, ...)                  do { LOG_ERROR(message, ##__VA_ARGS__); } while (0)

#endif

template <typename T>
void quicksort (T* data, usize count, i32 (*compare)(const T*, const T*))
{
    quicksort_raw(data, count, sizeof(T), (i32 (*)(const void*, const void*))compare);
}
