#pragma once

// Shared by the platform files only: platform.cpp (every platform), platform_unix.cpp (Linux and
// macOS), platform_linux.cpp, platform_win32.cpp, platform_macos.mm. Everything else uses
// platform.h.

#include "platform.h"

// Windows and Linux run video on a window thread of puppetxr's own (platform.cpp): Windows
// delivers a window's messages only to the thread that created it and pumping takes every message
// of the calling thread, and the app's main thread is the app's to block. macOS keeps Cocoa on the
// app's main thread (platform_macos.mm).
#define HAVE_WINDOW_THREAD (HAVE_WINDOWS || HAVE_LINUX)

#define OS_MAX_WINDOWS 16

//
// Video drivers
//

struct OS_VideoDriver;

// What every driver's window starts with; the driver keeps its own state behind `data`. Fields
// change on the video thread only.
struct OS_Window
{
    const OS_VideoDriver* driver;
    void*                 data;
    u32                   id;
    u32                   width;   // pixels
    u32                   height;
    f32                   density; // pixels per point
    b8                    hidden;
    b8                    relative;
    OS_Cursor             cursor;
};

struct OS_WindowDesc
{
    const char* title;
    u32         width;      // points
    u32         height;
    b8          vulkan;
    u32         x11_visual; // 0 for the default
    b8          hidden;
};

// One table per driver. Every entry runs on the video thread; a null entry is a feature the
// driver does not have. Windows passed in are never null.
struct OS_VideoDriver
{
    const char*        name;
    b8                 (*init)                  (); // false when there is no display for it
    void               (*quit)                  ();
    b8                 (*create_window)         (OS_Window* window, const OS_WindowDesc& desc); // fills width, height, density, hidden
    void               (*destroy_window)        (OS_Window* window);
    b8                 (*native_handles)        (OS_Window* window, OS_WindowNative& out_native);
    f32                (*refresh_rate)          (OS_Window* window);
    const char* const* (*vulkan_extensions)     (u32& out_count);
    b8                 (*create_vulkan_surface) (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface);
    void*              (*metal_layer)           (OS_Window* window);
    void               (*set_relative_mouse)    (OS_Window* window, b8 relative);
    b8                 (*maximize)              (OS_Window* window);
    b8                 (*maximized)             (OS_Window* window);
    void               (*show)                  (OS_Window* window); // and raise
    void               (*hide)                  (OS_Window* window);
    void               (*set_cursor)            (OS_Window* window, OS_Cursor cursor);
    void               (*text_input)            (OS_Window* window, b8 active, f32 x, f32 y, f32 height);
    b8                 (*clipboard_text)        (OS_Window* window, char* out_text, usize size);
    void               (*set_clipboard_text)    (OS_Window* window, const char* text);
    b8                 (*cursor_position)       (OS_Window* window, f32& out_x, f32& out_y);
    void               (*keep_cursor)           (OS_Window* window, b8 held);
    b8                 (*key_label)             (OS_Key key, char* out, usize size);
    void               (*pump)                  (); // moves what the window system has into the queue, never blocks
    void               (*wait)                  (u64 timeout_ns); // window thread: sleeps until the window system has events, os_sys_wake_signal or the timeout, then clears the wake
};

extern const OS_VideoDriver os_offscreen_driver;
#if HAVE_LINUX
extern const OS_VideoDriver os_wayland_driver;
extern const OS_VideoDriver os_x11_driver;
#endif
#if HAVE_WINDOWS
extern const OS_VideoDriver os_windows_driver;
#endif
#if HAVE_MACOS
extern const OS_VideoDriver os_cocoa_driver;
#endif

// Video thread: hands an event to os_poll_events. The window id is filled in from `window`.
void          os_queue_event        (OS_Window* window, OS_RawEvent event);
void          os_queue_resize       (OS_Window* window, u32 width, u32 height, f32 density); // records the new size and queues the event when it changed
OS_Window*    os_window_of_id       (u32 id);
OS_Window*    os_window_at          (u32 index); // the index-th slot of the window table, null when free; index < OS_MAX_WINDOWS
const char*   os_key_ascii_label    (OS_Key key); // the label of a US layout, for drivers without one

//
// Per platform (platform_unix.cpp, platform_win32.cpp)
//

b8            os_sys_mutex_try_lock (OS_Mutex* mutex);
void          os_sys_mutex_lock     (OS_Mutex* mutex);
void          os_sys_mutex_unlock   (OS_Mutex* mutex);
b8            os_sys_semaphore_wait_ns (OS_Semaphore* semaphore, u64 timeout_ns); // false at the timeout
void          os_sys_log_write      (OS_LogLevel level, const char* text);       // stderr, and the debugger's output on Windows
void          os_sys_thread_name    (char* out_name, usize size);                 // the calling thread's, empty when it has none

#if HAVE_WINDOW_THREAD
// Wakes the window thread: level-triggered, set from any thread until the window thread clears it
void          os_sys_wake_open      ();
void          os_sys_wake_close     ();
void          os_sys_wake_signal    ();
void          os_sys_wake_clear     ();
void          os_sys_wake_wait      (u64 timeout_ns); // returns at once while set, then clears it
intptr_t      os_sys_wake_handle    ();               // the pipe's read end on Linux, the event's HANDLE on Windows
#endif

// The C runtime's file descriptors, for capturing stdout and stderr
b8            os_sys_pipe           (i32 out_fds[2]); // the read end non-blocking where it can be
i32           os_sys_fd_dup         (i32 fd);
void          os_sys_fd_dup2        (i32 from, i32 to);
void          os_sys_fd_close       (i32 fd);
void          os_sys_fd_write       (i32 fd, const void* data, usize size);
i64           os_sys_fd_read_available (i32 fd, void* data, usize capacity);

#if HAVE_WINDOWS
b8            os_win_to_wide        (const char* text, wchar_t* out, usize capacity);
b8            os_win_from_wide      (const wchar_t* text, char* out, usize capacity);
#endif

#if HAVE_MACOS
// The main thread's call queue (platform_macos.mm)
b8            os_mac_on_main_thread ();
void          os_mac_queue_create   ();
b8            os_mac_run_on_main    (OS_WindowThreadFunction* function, void* user, u32 timeout_ms); // false when the call did not run
void          os_mac_post_to_main   (OS_WindowThreadFunction* function, void* user);
void          os_mac_serve          (u64 ns);
void          os_mac_lock_on_main   (OS_Mutex* mutex); // serves calls while it waits
void          os_mac_unlocked       ();
#endif
