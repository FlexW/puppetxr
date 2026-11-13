#include "base.h"

#include <gtest/gtest.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#if defined(_WIN32)
#include <sys/timeb.h>
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#if defined(__linux__)
// SDL's x11 driver unloads libX11 at its shutdown, and the leaks of libX11's per display state
// then have no module name for tests/lsan.supp to match; every test process keeps it loaded
[[maybe_unused]] static const OS_Library* const keep_libx11 = os_library_open("libX11.so.6");
#endif

// Linux, or Wine, whose windows are X11 windows too
static b8 on_x11 ()
{
#if defined(_WIN32)
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
#elif defined(__linux__)
    return true;
#else
    return false;
#endif
}

// The temporary directory of the platform, for files the tests write
static const char* temp_dir ()
{
    static char dir[OS_MAX_PATH] = {};
    if (!dir[0] && !os_temp_dir(dir, sizeof(dir)))
    {
        cstr_copy(dir, sizeof(dir), ".");
    }
    return dir;
}

namespace
{

struct Point
{
    i32 x;
    i32 y;
};

} // namespace

TEST(Base, array_add_remove)
{
    Array<i32> a = {};
    EXPECT_EQ(a.length, 0u);

    a.add(1);
    a.add(2);
    a.add(3);
    EXPECT_EQ(a.length, 3u);
    EXPECT_EQ(a[0], 1);
    EXPECT_EQ(a.last(), 3);

    a.remove(1);
    EXPECT_EQ(a.length, 2u);
    EXPECT_EQ(a[1], 3);

    a.remove_swap(0);
    EXPECT_EQ(a.length, 1u);
    EXPECT_EQ(a[0], 3);

    a.pop();
    EXPECT_EQ(a.length, 0u);

    a.release();
    EXPECT_EQ(a.data, nullptr);
    EXPECT_EQ(a.allocated, 0u);
}

TEST(Base, array_insert_and_bulk_add)
{
    Array<i32> a = {};
    const i32  items[] = { 10, 20, 30 };
    a.add(items, 3);
    a.insert(15, 1);
    EXPECT_EQ(a.length, 4u);
    EXPECT_EQ(a[0], 10);
    EXPECT_EQ(a[1], 15);
    EXPECT_EQ(a[2], 20);
    EXPECT_EQ(a[3], 30);

    const i32& added = a.add();
    EXPECT_EQ(added, 0);
    EXPECT_EQ(a.length, 5u);

    usize index = 0;
    EXPECT_TRUE(a.contains(20, &index));
    EXPECT_EQ(index, 2u);
    EXPECT_FALSE(a.contains(99, &index));
    a.release();
}

TEST(Base, array_grows_past_initial_capacity)
{
    Array<Point> a = {};
    for (i32 i = 0; i < 1000; i++)
    {
        a.add({ .x = i, .y = -i });
    }
    EXPECT_EQ(a.length, 1000u);
    for (i32 i = 0; i < 1000; i++)
    {
        EXPECT_EQ(a[(usize)i].x, i);
        EXPECT_EQ(a[(usize)i].y, -i);
    }

    i32 sum = 0;
    for (const Point& p : a)
    {
        sum += p.x;
    }
    EXPECT_EQ(sum, 499500);
    a.release();
}

TEST(Base, array_reserve_and_resize)
{
    Array<u8> a = {};
    a.reserve(64);
    EXPECT_GE(a.allocated, 64u);
    EXPECT_EQ(a.length, 0u);
    a.resize(10);
    EXPECT_EQ(a.length, 10u);
    EXPECT_EQ(a[9], 0);
    a.clear();
    EXPECT_EQ(a.length, 0u);
    EXPECT_GE(a.allocated, 64u);
    a.release();
}

TEST(Base, handle_null_and_equality)
{
    const Handle<Point> a = {};
    const Handle<Point> b = NULL_HANDLE;
    EXPECT_FALSE(a);
    EXPECT_FALSE(b);
    EXPECT_EQ(a, b);

    Handle<Point> const c(42);
    EXPECT_TRUE(c);
    EXPECT_NE(a, c);
}

TEST(Base, sparse_array_add_get_remove)
{
    SparseArray<Point> s = {};

    const Handle<Point> h1 = s.add({ .x = 1, .y = 2 });
    const Handle<Point> h2 = s.add({ .x = 3, .y = 4 });
    EXPECT_EQ(s.length(), 2u);
    EXPECT_TRUE(s.is_valid(h1));
    EXPECT_TRUE(s.is_valid(h2));
    EXPECT_EQ(s.get(h1).x, 1);
    EXPECT_EQ(s.get(h2).y, 4);

    s.remove(h1);
    EXPECT_FALSE(s.is_valid(h1));
    EXPECT_TRUE(s.is_valid(h2));
    EXPECT_EQ(s.length(), 1u);
    EXPECT_EQ(s.try_get(h1), nullptr);
    EXPECT_EQ(s.try_get(h2)->x, 3);

    const Handle<Point> h3 = s.add({ .x = 5, .y = 6 });
    EXPECT_NE(h3, h1);
    EXPECT_FALSE(s.is_valid(h1));
    EXPECT_TRUE(s.is_valid(h3));
    EXPECT_EQ(s.get(h3).x, 5);

    s.release();
}

struct PointTag;

// A backend stores its own struct but hands out the handles of the public opaque type
TEST(Base, sparse_array_handles_carry_the_tag_type)
{
    SparseArray<Point, PointTag> s = {};
    const Handle<PointTag> h = s.add({ .x = 7, .y = 8 });
    EXPECT_TRUE(s.is_valid(h));
    EXPECT_EQ(s.get(h).y, 8);
    EXPECT_EQ(s.handle_at(0), h);
    s.remove(h);
    EXPECT_FALSE(s.is_valid(h));
    s.release();
}

TEST(Base, sparse_array_rejects_garbage_handles)
{
    SparseArray<Point> s = {};
    s.add({ .x = 1, .y = 1 });

    EXPECT_FALSE(s.is_valid(NULL_HANDLE));
    EXPECT_FALSE(s.is_valid(Handle<Point>(0xDEADBEEFCAFEBABEull)));
    EXPECT_FALSE(s.is_valid(Handle<Point>(0xFFFFFFFFFFFFFFFFull)));
    EXPECT_FALSE(s.is_valid(Handle<Point>(1ull << 32)));
    EXPECT_EQ(s.try_get(Handle<Point>(12345)), nullptr);

    s.remove(Handle<Point>(0xDEADBEEFCAFEBABEull));
    EXPECT_EQ(s.length(), 1u);
    s.release();
}

TEST(Base, sparse_array_iteration_and_handle_at)
{
    SparseArray<Point> s = {};
    for (i32 i = 0; i < 10; i++)
    {
        s.add({ .x = i, .y = 0 });
    }
    s.remove(s.handle_at(3));
    EXPECT_EQ(s.length(), 9u);

    i32 sum = 0;
    for (const Point& p : s)
    {
        sum += p.x;
    }
    EXPECT_EQ(sum, 45 - 3);

    for (usize i = 0; i < s.length(); i++)
    {
        EXPECT_TRUE(s.is_valid(s.handle_at(i)));
        EXPECT_EQ(&s.get(s.handle_at(i)), &s.data()[i]);
    }
    s.release();
}

TEST(Base, hash_map_insert_get_erase)
{
    HashMap<u64, i32> m = {};
    EXPECT_FALSE(m.contains(1));

    m.insert(1, 100);
    m.insert(2, 200);
    EXPECT_TRUE(m.contains(1));
    EXPECT_EQ(m.get(1), 100);
    EXPECT_EQ(m[2], 200);
    EXPECT_EQ(m.used, 2u);

    m[2] = 250;
    EXPECT_EQ(m.get(2), 250);

    EXPECT_TRUE(m.erase(1));
    EXPECT_FALSE(m.erase(1));
    EXPECT_FALSE(m.contains(1));
    EXPECT_EQ(m.at(1), nullptr);
    EXPECT_EQ(m.used, 1u);

    m.release();
}

TEST(Base, hash_map_grows_and_keeps_entries)
{
    HashMap<u64, u64> m = {};
    for (u64 i = 1; i <= 5000; i++)
    {
        m.insert(i * 7919, i);
    }
    EXPECT_EQ(m.used, 5000u);
    for (u64 i = 1; i <= 5000; i++)
    {
        ASSERT_TRUE(m.contains(i * 7919));
        EXPECT_EQ(m.get(i * 7919), i);
    }
    for (u64 i = 1; i <= 5000; i += 2)
    {
        EXPECT_TRUE(m.erase(i * 7919));
    }
    for (u64 i = 1; i <= 5000; i++)
    {
        EXPECT_EQ(m.contains(i * 7919), (i % 2) == 0);
    }
    m.release();
}

TEST(Base, cstr_helpers)
{
    EXPECT_TRUE(cstr_equal("abc", "abc"));
    EXPECT_FALSE(cstr_equal("abc", "abd"));
    EXPECT_FALSE(cstr_equal("abc", nullptr));
    EXPECT_TRUE(cstr_equal(nullptr, nullptr));
    EXPECT_EQ(cstr_length("hello"), 5u);
    EXPECT_EQ(cstr_length(nullptr), 0u);
    EXPECT_TRUE(cstr_starts_with("/user/hand/left/input", "/user/hand/left"));
    EXPECT_FALSE(cstr_starts_with("/user/hand", "/user/hand/left"));

    char buf[4] = {};
    EXPECT_FALSE(cstr_copy(buf, sizeof(buf), "toolong"));
    EXPECT_TRUE(cstr_equal(buf, "too"));
    EXPECT_TRUE(cstr_copy(buf, sizeof(buf), "ok"));
    EXPECT_TRUE(cstr_equal(buf, "ok"));
    EXPECT_TRUE(cstr_copy(buf, sizeof(buf), buf));
    EXPECT_TRUE(cstr_equal(buf, "ok"));

    char fmt[16] = {};
    EXPECT_EQ(cstr_format(fmt, sizeof(fmt), "%d-%s", 7, "x"), 3u);
    EXPECT_TRUE(cstr_equal(fmt, "7-x"));
}

TEST(Base, hashes_are_stable)
{
    EXPECT_EQ(hash_cstr(""), 0xCBF29CE484222325ull);
    EXPECT_EQ(hash_cstr("a"), 0xAF63DC4C8601EC8Cull);
    EXPECT_EQ(hash_cstr("hello"), hash((const u8*)"hello", 5));
    EXPECT_EQ(hash_u32((const u8*)"a", 1), 0xE40C292Cu);
    EXPECT_EQ(crc32((const u8*)"123456789", 9), 0xCBF43926u);
    EXPECT_EQ(crc32(nullptr, 0), 0u);
}

TEST(Base, defer_runs_at_scope_exit_in_reverse_order)
{
    i32 order[2] = {};
    i32 counter  = 0;
    {
        defer { order[1] = ++counter; };
        defer { order[0] = ++counter; };
        EXPECT_EQ(counter, 0);
    }
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

TEST(Base, quicksort_sorts)
{
    i32 values[] = { 5, 3, 9, 1, 4, 1 };
    quicksort<i32>(values, ARRAY_SIZE(values), [](const i32* a, const i32* b) -> i32 { return (*a > *b) - (*a < *b); });
    const i32 expected[] = { 1, 1, 3, 4, 5, 9 };
    for (usize i = 0; i < ARRAY_SIZE(values); i++)
    {
        EXPECT_EQ(values[i], expected[i]);
    }
}

#if defined(__APPLE__) && !PUPPETXR_HEADLESS_ONLY
// Regression: the SDL linked into puppetxr registered its Objective-C classes under SDL's own names,
// and the Objective-C runtime keeps one class per name for the whole process. An app with its own
// SDL3 got "Class SDL3Application is implemented in both" and either SDL could end up with the
// other's classes
TEST(Platform, sdl_objective_c_classes_carry_the_puppetxr_prefix)
{
    for (const char* name : { "SDL3Application", "SDL3AppDelegate", "SDL3Window", "SDL3View", "SDL3Cocoa_WindowListener", "SDL_CocoaVideoData" })
    {
        EXPECT_EQ(objc_getClass(name), nullptr) << name;
    }
    EXPECT_NE(objc_getClass("PUPPETXR_SDL3Application"), nullptr);
    EXPECT_NE(objc_getClass("PUPPETXR_SDL3Window"), nullptr);
}
#endif

TEST(Platform, empty_memory_ranges_may_be_null)
{
    u8 byte = 7;
    EXPECT_EQ(memory_copy(nullptr, nullptr, 0), nullptr);
    EXPECT_EQ(memory_move(&byte, nullptr, 0), &byte);
    EXPECT_EQ(memory_zero(nullptr, 0), nullptr);
    EXPECT_TRUE(memory_equal(nullptr, &byte, 0));
    EXPECT_EQ(byte, 7);
}

namespace
{

struct ThreadHandover
{
    OS_Semaphore* ready;
    OS_Semaphore* done;
    u32           value;
};

} // namespace

static i32 thread_handover (void* user)
{
    ThreadHandover& handover = *(ThreadHandover*)user;
    os_semaphore_wait(handover.ready);
    handover.value *= 2;
    os_semaphore_signal(handover.done);
    return 7;
}

TEST(Platform, threads_join_and_semaphores_hand_over)
{
    ThreadHandover handover = { .ready = os_semaphore_create(0), .done = os_semaphore_create(0), .value = 0 };
    ASSERT_NE(handover.ready, nullptr);
    ASSERT_NE(handover.done, nullptr);

    OS_Thread* thread = os_thread_create("handover", thread_handover, &handover);
    ASSERT_NE(thread, nullptr);
    EXPECT_FALSE(os_semaphore_try_wait(handover.done));
    handover.value = 21;
    os_semaphore_signal(handover.ready);
    os_semaphore_wait(handover.done);
    EXPECT_EQ(handover.value, 42u);
    EXPECT_EQ(os_thread_join(thread), 7);

    os_semaphore_signal(handover.done);
    EXPECT_TRUE(os_semaphore_try_wait(handover.done));
    EXPECT_FALSE(os_semaphore_try_wait(handover.done));
    os_semaphore_destroy(handover.ready);
    os_semaphore_destroy(handover.done);
}

TEST(Platform, environment_set_and_unset)
{
    os_set_env("PUPPETXR_TEST_ENVIRONMENT", "1");
    ASSERT_NE(os_env("PUPPETXR_TEST_ENVIRONMENT"), nullptr);
    EXPECT_TRUE(cstr_equal(os_env("PUPPETXR_TEST_ENVIRONMENT"), "1"));
    os_unset_env("PUPPETXR_TEST_ENVIRONMENT");
    EXPECT_EQ(os_env("PUPPETXR_TEST_ENVIRONMENT"), nullptr);
}

// XR_KHR_convert_timespec_time speaks the clock apps read timespecs from: CLOCK_MONOTONIC, and on
// Windows the UTC wall clock of timespec_get, as the CTS and Meta's MRUK read it there
TEST(Platform, timespec_clock_is_the_one_apps_read)
{
#if defined(_WIN32)
    // timespec_get(TIME_UTC) where the C runtime has it; mingw's has the same clock only in ms
    __timeb64 now = {};
    _ftime64(&now);
    const i64 theirs = (i64)now.time * 1000000000ll + (i64)now.millitm * 1000000ll;
#else
    timespec now = {};
    ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &now), 0);
    const i64 theirs = (i64)now.tv_sec * 1000000000ll + (i64)now.tv_nsec;
#endif
    const i64 ours   = (i64)os_timespec_ns();
    EXPECT_LT(ours - theirs, 100000000ll);
    EXPECT_GT(ours - theirs, -100000000ll);
}

// Graphics backends load their entry points at runtime, from libraries the app already loaded
TEST(Platform, library_symbols_resolve_in_the_process)
{
    EXPECT_NE(os_library_symbol(nullptr, "malloc"), nullptr);
    EXPECT_EQ(os_library_symbol(nullptr, "puppetxr_no_such_symbol"), nullptr);
    EXPECT_EQ(os_library_symbol(nullptr, nullptr), nullptr);
    EXPECT_EQ(os_library_open("libpuppetxr_no_such_library.so"), nullptr);
    EXPECT_EQ(os_library_open(nullptr), nullptr);
    os_library_close(nullptr);
#if defined(__linux__)
    OS_Library* libc = os_library_open("libc.so.6");
    ASSERT_NE(libc, nullptr);
    EXPECT_NE(os_library_symbol(libc, "malloc"), nullptr); // not compared with the process's: sanitizers interpose it
    EXPECT_EQ(os_library_symbol(libc, "puppetxr_no_such_symbol"), nullptr);
    os_library_close(libc);
#endif
}

// An app may load its Vulkan loader from its bundle, off dyld's search path and under the file
// name of its version; the runtime finds it by its install name
TEST(Platform, loaded_libraries_are_found_by_install_name_off_the_search_path)
{
#if HAVE_MACOS
    if (!PUPPETXR_TEST_VULKAN_LOADER[0])
    {
        GTEST_SKIP() << "no Vulkan loader";
    }
    char resolved[OS_MAX_PATH];
    ASSERT_NE(realpath(PUPPETXR_TEST_VULKAN_LOADER, resolved), nullptr);
    char bundle[OS_MAX_PATH];
    ASSERT_TRUE(os_create_temp_directory("puppetxr_bundle", bundle, sizeof(bundle)));
    defer { os_delete_tree(bundle); };
    char copied[OS_MAX_PATH];
    cstr_format(copied, sizeof(copied), "%s/libvulkan.1.999.0.dylib", bundle);
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(resolved, &data, &size));
    ASSERT_TRUE(os_write_file(copied, data, size));
    os_memory_release(data);

    char found[OS_MAX_PATH];
    void* app_loader = dlopen(copied, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(app_loader, nullptr) << dlerror();
    ASSERT_TRUE(os_library_path("libvulkan.1.dylib", found, sizeof(found)));
    EXPECT_TRUE(cstr_find(found, "libvulkan.1.") != nullptr) << found;
    EXPECT_TRUE(os_module_loaded("libvulkan.1.dylib"));
    OS_Library* library = os_library_open("libvulkan.1.dylib");
    ASSERT_NE(library, nullptr);
    EXPECT_NE(os_library_symbol(library, "vkGetInstanceProcAddr"), nullptr);
    os_library_close(library);
    dlclose(app_loader);
    EXPECT_FALSE(os_library_path("libpuppetxr_no_such_library.dylib", found, sizeof(found)));
#else
    char found[OS_MAX_PATH];
    EXPECT_FALSE(os_library_path("libc.so.6", found, sizeof(found)));
#endif
}

// Engine plugins that need runtime workarounds are recognised by their library in the process
TEST(Platform, loaded_modules_are_found_without_loading_them)
{
    EXPECT_FALSE(os_module_loaded(nullptr));
    EXPECT_FALSE(os_module_loaded(""));
    EXPECT_FALSE(os_module_loaded("puppetxr_no_such_library.dll"));
    EXPECT_FALSE(os_module_loaded("libpuppetxr_no_such_library.so"));
#if defined(_WIN32)
    EXPECT_TRUE(os_module_loaded("kernel32.dll"));
    EXPECT_FALSE(os_module_loaded("msftedit.dll"));
    EXPECT_FALSE(os_module_loaded("msftedit.dll")) << "asking must not load it";
#elif defined(__linux__)
    EXPECT_TRUE(os_module_loaded("libc.so.6"));
#endif
}

TEST(Platform, module_directory_is_where_this_code_was_loaded_from)
{
    char dir[OS_MAX_PATH] = {};
    ASSERT_TRUE(os_module_directory(dir, sizeof(dir)));
    // PUPPETXR_TESTS_PATH is this binary, with its extension on platforms that have one
    const char* name = PUPPETXR_TESTS_PATH;
    for (const char* at = PUPPETXR_TESTS_PATH; *at; at++)
    {
        name = *at == '/' || *at == '\\' ? at + 1 : name;
    }
    char binary[OS_MAX_PATH] = {};
    cstr_format(binary, sizeof(binary), "%s/%s", dir, name);
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(binary, &data, &size));
    os_memory_release(data);
    EXPECT_FALSE(os_module_directory(dir, 4));
}

// Unreal clips the cursor to its game window while that is in front; the mirror window keeps its
// own clip while it holds the mouse and frees the cursor while it is in front without
TEST(Platform, cursor_clip_follows_the_held_mouse)
{
    const OS_ScreenRect desktop = { 0, 0, 3440, 1440 };
    const OS_ScreenRect client  = { 100, 100, 900, 700 };
    const OS_ScreenRect center  = { 499, 399, 501, 401 };
    const OS_ScreenRect unreal  = { 1400, 456, 2040, 936 };
    EXPECT_EQ(os_cursor_clip_action(true, true, center, client, desktop), OS_ClipAction::Keep) << "SDL's relative mode";
    EXPECT_EQ(os_cursor_clip_action(true, true, client, client, desktop), OS_ClipAction::Keep);
    EXPECT_EQ(os_cursor_clip_action(true, true, desktop, client, desktop), OS_ClipAction::Clip) << "another window lifted the clip";
    EXPECT_EQ(os_cursor_clip_action(true, false, unreal, client, desktop), OS_ClipAction::Clip);
    EXPECT_EQ(os_cursor_clip_action(false, false, center, client, desktop), OS_ClipAction::Free) << "left over from holding";
    EXPECT_EQ(os_cursor_clip_action(false, true, unreal, client, desktop), OS_ClipAction::Free) << "a cage in a window behind this one";
    EXPECT_EQ(os_cursor_clip_action(false, false, unreal, client, desktop), OS_ClipAction::Keep) << "that window is in front";
    EXPECT_EQ(os_cursor_clip_action(false, true, desktop, client, desktop), OS_ClipAction::Keep);
}

TEST(Platform, key_names_round_trip)
{
    for (u32 i = 0; i < (u32)OS_Key::Count; i++)
    {
        OS_Key key = OS_Key::None;
        EXPECT_TRUE(os_key_from_name(os_key_name((OS_Key)i), key)) << os_key_name((OS_Key)i);
        EXPECT_EQ((u32)key, i);
    }
    OS_Key key = OS_Key::A;
    EXPECT_FALSE(os_key_from_name("not_a_key", key));
    EXPECT_EQ(key, OS_Key::None);
    EXPECT_TRUE(cstr_equal(os_key_name(OS_Key::MouseLeft), "mouse_left"));
    EXPECT_TRUE(cstr_equal(os_key_name(OS_Key::LeftBracket), "left_bracket"));
    EXPECT_TRUE(cstr_equal(os_key_name(OS_Key::RightBracket), "right_bracket"));
    EXPECT_TRUE(cstr_equal(os_key_name((OS_Key)255), "none"));
}

// Shipped builds never stop the host app on an internal assert
TEST(Platform, dist_builds_log_asserts_by_default)
{
    EXPECT_EQ(os_default_assert_mode(), PUPPETXR_DIST ? OS_AssertMode::Log : OS_AssertMode::Break);
}

TEST(Platform, assert_log_mode_counts_instead_of_breaking)
{
    os_set_assert_mode(OS_AssertMode::Log);
    const u32 before = os_assert_count();
    ASSERT(1 == 2);
    ASSERT_MSG(false, "with %d", 1);
    FAIL_MSG("failure %s", "x");
    EXPECT_EQ(os_assert_count(), before + 3);
}

TEST(Platform, events_translate_from_native_events)
{
    os_set_env("SDL_VIDEODRIVER", "offscreen");
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }

    OS_Window* window = os_create_window("test", 320, 200, false);
    ASSERT_NE(window, nullptr);
    u32 width = 0;
    u32 height = 0;
    os_window_size(window, width, height);
    EXPECT_EQ(width, 320u);
    EXPECT_EQ(height, 200u);

    OS_Event drain[64];
    os_poll_events(drain, 64);

    SDL_Event key = {};
    key.type         = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_W;
    SDL_PushEvent(&key);
    SDL_Event repeat = key;
    repeat.key.repeat = true;
    SDL_PushEvent(&repeat);
    SDL_Event button = {};
    button.type          = SDL_EVENT_MOUSE_BUTTON_UP;
    button.button.button = SDL_BUTTON_RIGHT;
    SDL_PushEvent(&button);
    SDL_Event motion = {};
    motion.type        = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 3.0f;
    motion.motion.yrel = -2.0f;
    SDL_PushEvent(&motion);
    SDL_Event grave = {};
    grave.type         = SDL_EVENT_KEY_UP;
    grave.key.scancode = SDL_SCANCODE_GRAVE;
    SDL_PushEvent(&grave);
    SDL_Event bracket = {};
    bracket.type         = SDL_EVENT_KEY_DOWN;
    bracket.key.scancode = SDL_SCANCODE_RIGHTBRACKET;
    SDL_PushEvent(&bracket);
    SDL_Event wheel = {};
    wheel.type            = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y         = 2.0f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    SDL_PushEvent(&wheel);
    SDL_Event focus = {};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    SDL_PushEvent(&focus);
    SDL_Event quit = {};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);

    OS_Event  events[9] = {};
    const u32 count     = os_poll_events(events, 9);
    ASSERT_EQ(count, 8u);
    EXPECT_EQ(events[0].type, OS_EventType::KeyDown);
    EXPECT_EQ(events[0].key, OS_Key::W);
    EXPECT_EQ(events[1].type, OS_EventType::KeyUp);
    EXPECT_EQ(events[1].key, OS_Key::MouseRight);
    EXPECT_EQ(events[2].type, OS_EventType::MouseMotion);
    EXPECT_FLOAT_EQ(events[2].dx, 3.0f);
    EXPECT_FLOAT_EQ(events[2].dy, -2.0f);
    EXPECT_EQ(events[3].type, OS_EventType::KeyUp);
    EXPECT_EQ(events[3].key, OS_Key::Grave);
    EXPECT_EQ(events[4].type, OS_EventType::KeyDown);
    EXPECT_EQ(events[4].key, OS_Key::RightBracket);
    EXPECT_EQ(events[5].type, OS_EventType::MouseWheel);
    EXPECT_FLOAT_EQ(events[5].dy, -2.0f) << "a flipped wheel turns the other way";
    EXPECT_EQ(events[6].type, OS_EventType::FocusLost);
    EXPECT_EQ(events[7].type, OS_EventType::WindowClose);

    os_destroy_window(window);
    os_shutdown();
    os_unset_env("SDL_VIDEODRIVER");
}

// The mirror window stays hidden until it has a frame, instead of showing what Windows leaves
// in an unpainted window for the seconds an engine takes to start
// A click on the unfocused mirror window acts on the panels; it used to only focus the window
TEST(Platform, a_click_that_focuses_the_window_also_clicks)
{
    os_set_env("SDL_VIDEODRIVER", "offscreen");
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer {
        os_shutdown();
        os_unset_env("SDL_VIDEODRIVER");
    };
    EXPECT_TRUE(SDL_GetHintBoolean(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, false));
}

// A call into the window thread wakes it: SDL's own wakeup was missed, and every frame waited out
// the window thread's 100 ms event wait, so apps ran at 10 frames a second without input
TEST(Platform, window_thread_calls_return_at_once)
{
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    OS_Window* window = os_create_window("test", 64, 64, false);
    ASSERT_NE(window, nullptr);
    defer { os_destroy_window(window); };
    OS_Event drain[64];
    os_poll_events(drain, 64);
    const u64 start  = os_time_ns();
    u32       width  = 0;
    u32       height = 0;
    for (u32 i = 0; i < 50; i++)
    {
        os_window_size(window, width, height);
    }
    EXPECT_LT(os_time_ns() - start, 500000000ull) << "50 calls, each at most 10 ms";
}

namespace
{

struct WindowCallProbe
{
    SDL_AtomicInt runs;
    SDL_AtomicInt on_window_thread;
};

struct WindowCallThread
{
    OS_Window*      window;
    WindowCallProbe probe;
    SDL_AtomicInt   done;
    u64             took_ns;
    OS_Mutex*       hold;     // held by the thread while it calls, when set
    OS_Semaphore*   holding;  // signalled once it holds `hold`
};

} // namespace

static void probe_window_thread (void* user)
{
    WindowCallProbe& probe = *(WindowCallProbe*)user;
    SDL_SetAtomicInt(&probe.on_window_thread, os_is_window_thread() ? 1 : 0);
    SDL_AddAtomicInt(&probe.runs, 1);
}

#if HAVE_MACOS
static i32 call_from_another_thread (void* user)
{
    WindowCallThread& call = *(WindowCallThread*)user;
    if (call.hold)
    {
        os_mutex_lock(call.hold);
        os_semaphore_signal(call.holding);
    }
    const u64 start = os_monotonic_ns();
    os_run_on_window_thread(probe_window_thread, &call.probe);
    call.took_ns = os_monotonic_ns() - start;
    if (call.hold)
    {
        os_mutex_unlock(call.hold);
    }
    SDL_SetAtomicInt(&call.done, 1);
    return 0;
}
#endif

// Frames run on threads of the app while its main thread may be blocked on them; a window call
// posted from such a thread runs on the window thread later and never waits
TEST(Platform, posted_window_thread_calls_run_at_once)
{
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    WindowCallProbe probe = {};
    os_post_to_window_thread(probe_window_thread, &probe);
    if (os_is_window_thread())
    {
        EXPECT_EQ(SDL_GetAtomicInt(&probe.runs), 1) << "on the window thread a post runs at once";
    }
    const u64 start = os_monotonic_ns();
    while (SDL_GetAtomicInt(&probe.runs) == 0 && os_monotonic_ns() - start < 1000000000ull)
    {
        os_serve_posted(1000000);
    }
    EXPECT_EQ(SDL_GetAtomicInt(&probe.runs), 1);
    EXPECT_EQ(SDL_GetAtomicInt(&probe.on_window_thread), 1);
    EXPECT_LT(os_monotonic_ns() - start, 100000000ull);
}

// Cocoa wants windows on the main thread, which belongs to the app: calls from other threads run
// there whenever it comes by, and wait for it at most OS_MAIN_THREAD_TIMEOUT_MS
TEST(Platform, window_calls_from_other_threads_run_on_the_main_thread)
{
#if HAVE_MACOS
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    ASSERT_TRUE(os_is_window_thread());
    WindowCallThread call = {};
    OS_Thread* thread = os_thread_create("caller", call_from_another_thread, &call);
    while (!SDL_GetAtomicInt(&call.done))
    {
        os_serve_posted(1000000);
    }
    os_thread_join(thread);
    EXPECT_EQ(SDL_GetAtomicInt(&call.probe.runs), 1);
    EXPECT_EQ(SDL_GetAtomicInt(&call.probe.on_window_thread), 1);
    EXPECT_LT(call.took_ns, 100000000ull);
#else
    GTEST_SKIP() << "macOS only";
#endif
}

// Unity's main thread blocks on its render thread, which makes the window calls; waiting for the
// main thread deadlocked, so a call it does not start in time is withdrawn and the next ones fail
// at once until it comes by again
TEST(Platform, window_calls_the_main_thread_never_takes_are_withdrawn)
{
#if HAVE_MACOS
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    WindowCallThread first = {};
    OS_Thread* thread = os_thread_create("caller", call_from_another_thread, &first);
    os_thread_join(thread);
    EXPECT_EQ(SDL_GetAtomicInt(&first.probe.runs), 0);
    EXPECT_GE(first.took_ns, (u64)OS_MAIN_THREAD_TIMEOUT_MS * 1000000ull);
    EXPECT_LT(first.took_ns, (u64)OS_MAIN_THREAD_TIMEOUT_MS * 1000000ull + 200000000ull);
    ASSERT_GT(os_log_history_count(), 0u);
    EXPECT_NE(cstr_find(os_log_history(os_log_history_count() - 1)->text, OS_WITHDRAWN_CALL_LOG), nullptr) << "OxrThreadWindowedTest counts these lines";

    WindowCallThread stalled = {};
    thread = os_thread_create("caller", call_from_another_thread, &stalled);
    os_thread_join(thread);
    EXPECT_EQ(SDL_GetAtomicInt(&stalled.probe.runs), 0);
    EXPECT_LT(stalled.took_ns, 10000000ull) << "a stalled main thread fails calls at once";

    os_serve_posted(0);
    WindowCallThread again = {};
    thread = os_thread_create("caller", call_from_another_thread, &again);
    while (!SDL_GetAtomicInt(&again.done))
    {
        os_serve_posted(1000000);
    }
    os_thread_join(thread);
    EXPECT_EQ(SDL_GetAtomicInt(&again.probe.runs), 1) << "calls run again once the main thread came by";
#else
    GTEST_SKIP() << "macOS only";
#endif
}

// The main thread waiting for the runtime lock serves the window call of the thread holding it
TEST(Platform, a_main_thread_waiting_for_a_mutex_serves_window_calls)
{
#if HAVE_MACOS
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    OS_Mutex*        mutex = os_mutex_create();
    OS_Semaphore*    holding = os_semaphore_create(0);
    WindowCallThread call  = { .hold = mutex, .holding = holding };
    OS_Thread*       thread = os_thread_create("caller", call_from_another_thread, &call);
    os_semaphore_wait(holding);
    const u64 start = os_monotonic_ns();
    os_mutex_lock(mutex);
    const u64 waited = os_monotonic_ns() - start;
    os_mutex_unlock(mutex);
    os_thread_join(thread);
    EXPECT_EQ(SDL_GetAtomicInt(&call.probe.runs), 1);
    EXPECT_LT(waited, 100000000ull);
    os_semaphore_destroy(holding);
    os_mutex_destroy(mutex);
#else
    GTEST_SKIP() << "macOS only";
#endif
}

namespace
{

struct WindowThreadWait
{
    OS_Mutex*     mutex;
    OS_Semaphore* waiting;
    OS_Semaphore* done;
};

} // namespace

static void wait_for_mutex (void* user)
{
    WindowThreadWait& wait = *(WindowThreadWait*)user;
    os_semaphore_signal(wait.waiting);
    os_mutex_lock(wait.mutex);
    os_mutex_unlock(wait.mutex);
    os_semaphore_signal(wait.done);
}

// A frame's posted window tick waited on the window thread for the runtime lock, which
// xrDestroySession held while it waited for the window thread to shut the panels down
TEST(Platform, a_window_thread_waiting_for_a_mutex_runs_the_calls_of_its_holder)
{
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless() || os_is_window_thread())
    {
        GTEST_SKIP() << "no window thread of its own";
    }
    WindowThreadWait wait = { .mutex = os_mutex_create(), .waiting = os_semaphore_create(0), .done = os_semaphore_create(0) };
    os_mutex_lock(wait.mutex);
    os_post_to_window_thread(wait_for_mutex, &wait);
    os_semaphore_wait(wait.waiting);
    WindowCallProbe probe = {};
    os_run_on_window_thread(probe_window_thread, &probe);
    EXPECT_EQ(SDL_GetAtomicInt(&probe.runs), 1);
    EXPECT_EQ(SDL_GetAtomicInt(&probe.on_window_thread), 1);
    os_mutex_unlock(wait.mutex);
    os_semaphore_wait(wait.done);
    os_semaphore_destroy(wait.done);
    os_semaphore_destroy(wait.waiting);
    os_mutex_destroy(wait.mutex);
}

namespace
{

struct MutexHandOver
{
    OS_Mutex*     mutex;
    OS_Semaphore* holding;
    u64           released_ns;
};

} // namespace

static i32 hold_mutex_briefly (void* user)
{
    MutexHandOver& over = *(MutexHandOver*)user;
    os_mutex_lock(over.mutex);
    os_semaphore_signal(over.holding);
    os_sleep_ns(20000000);
    over.released_ns = os_monotonic_ns();
    os_mutex_unlock(over.mutex);
    return 0;
}

// The waiting main thread slept a millisecond between tries; the unlock wakes it now. A recursive
// mutex taken on a successful try in the spin was taken twice and deadlocked
TEST(Platform, a_waiting_main_thread_takes_a_mutex_once_it_is_free)
{
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    MutexHandOver over = { .mutex = os_mutex_create(), .holding = os_semaphore_create(0) };
    OS_Thread* thread = os_thread_create("holder", hold_mutex_briefly, &over);
    os_semaphore_wait(over.holding);
    os_mutex_lock(over.mutex);
    const u64 late = os_monotonic_ns() - over.released_ns;
    os_mutex_unlock(over.mutex);
    os_thread_join(thread);
    EXPECT_LT(late, 1000000ull) << "taken within a millisecond of its release";

    // Taken once by each lock: a second thread gets it after one unlock
    os_mutex_lock(over.mutex);
    os_mutex_unlock(over.mutex);
    thread = os_thread_create("holder", hold_mutex_briefly, &over);
    os_semaphore_wait(over.holding);
    os_thread_join(thread);
    os_semaphore_destroy(over.holding);
    os_mutex_destroy(over.mutex);
}

#if HAVE_MACOS
extern "C" void* SDL_GetVideoDevice    ();
extern "C" void  Cocoa_HandleKeyEvent  (void* video, void* event);
extern "C" void  Cocoa_HandleMouseEvent(void* video, void* event);
extern "C" void  CGEventSetIntegerValueField (void* event, u32 field, i64 value);
extern "C" bool  SDL_SetKeyboardFocus  (SDL_Window* window);
extern "C" void  SDL_SetMouseFocus     (SDL_Window* window);
// CoreFoundation's header clashes with base.h's Handle and Point
extern "C" void* CFRunLoopGetMain      ();
extern "C" void  CFRunLoopStop         (void* loop);
extern "C" i32   CFRunLoopRunInMode    (const void* mode, f64 seconds, unsigned char return_after_source);
extern "C" const void* const kCFRunLoopDefaultMode;

namespace
{

struct NSPointValue
{
    f64 x;
    f64 y;
};

template <typename R, typename... A>
R objc_send (const void* receiver, const char* selector, A... args)
{
    return ((R (*)(const void*, SEL, A...))objc_msgSend)(receiver, sel_registerName(selector), args...);
}

// Unity's editor and Godot make NSApp of a class of their own and dispatch the events themselves
struct PlatformAppEventsTest : testing::Test
{
    OS_Window* window = nullptr;
    void*      app    = nullptr;
    Class      sdl_class = nullptr;

    void SetUp () override
    {
        os_shutdown();
        ASSERT_TRUE(os_initialize(false));
        if (os_is_headless())
        {
            GTEST_SKIP() << "no display";
        }
        window = os_create_window("app events", 200, 150, false, 0, true);
        ASSERT_NE(window, nullptr);
        app       = objc_send<void*>(objc_getClass("NSApplication"), "sharedApplication");
        sdl_class = object_getClass((id)app);
        object_setClass((id)app, objc_getClass("NSApplication"));
        // What the window being in front gives it, which a test behind the terminal never gets
        SDL_SetKeyboardFocus((SDL_Window*)os_window_native(window));
        SDL_SetMouseFocus((SDL_Window*)os_window_native(window));
        OS_Event drain[64];
        os_poll_events(drain, 64);
    }

    void TearDown () override
    {
        if (app)
        {
            object_setClass((id)app, sdl_class);
        }
        os_destroy_window(window);
        os_shutdown();
    }

    void* ns_window () const
    {
        return SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window*)os_window_native(window)), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    }

    void* key_event (u16 key_code, b8 down) const
    {
        const i64   number = objc_send<i64>(ns_window(), "windowNumber");
        void* const chars  = objc_send<void*>(objc_getClass("NSString"), "stringWithUTF8String:", "w");
        return objc_send<void*>(objc_getClass("NSEvent"), "keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:charactersIgnoringModifiers:isARepeat:keyCode:",
                                (u64)(down ? 10 : 11), NSPointValue{ 10.0, 10.0 }, (u64)0, (f64)0.0, number, (void*)nullptr, chars, chars, (signed char)0, key_code);
    }

    // A relative move: the deltas are fields 4 and 5 of the CGEvent under it
    void* move_event (i64 dx, i64 dy) const
    {
        const i64   number = objc_send<i64>(ns_window(), "windowNumber");
        void* const event  = objc_send<void*>(objc_getClass("NSEvent"), "mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:",
                                              (u64)5, NSPointValue{ 100.0, 75.0 }, (u64)0, (f64)0.0, number, (void*)nullptr, (i64)0, (i64)0, (f32)0.0f);
        void* const cg     = objc_send<void*>(event, "CGEvent");
        CGEventSetIntegerValueField(cg, 4, dx);
        CGEventSetIntegerValueField(cg, 5, dy);
        return objc_send<void*>(objc_getClass("NSEvent"), "eventWithCGEvent:", cg);
    }

    void like_the_app (void* event) const
    {
        objc_send<void>(app, "sendEvent:", event);
    }

    void like_sdls_pump (void* event, b8 key) const
    {
        if (key)
        {
            Cocoa_HandleKeyEvent(SDL_GetVideoDevice(), event);
        }
        else
        {
            Cocoa_HandleMouseEvent(SDL_GetVideoDevice(), event);
        }
        objc_send<void>(app, "sendEvent:", event);
    }

    u32 count (OS_EventType type, OS_Key key = OS_Key::None) const
    {
        OS_Event  events[64];
        const u32 polled = os_poll_queued_events(events, 64);
        u32       found  = 0;
        for (u32 i = 0; i < polled; i++)
        {
            found += events[i].type == type && events[i].key == key;
        }
        return found;
    }
};

} // namespace

TEST_F(PlatformAppEventsTest, keys_the_app_dispatches_reach_the_window)
{
    like_the_app(key_event(13, true)); // kVK_ANSI_W
    EXPECT_EQ(count(OS_EventType::KeyDown, OS_Key::W), 1u);
    like_the_app(key_event(13, false));
    EXPECT_EQ(count(OS_EventType::KeyUp, OS_Key::W), 1u) << "a released key does not stay held";
}

TEST_F(PlatformAppEventsTest, keys_the_pump_dispatches_are_handled_once)
{
    like_sdls_pump(key_event(13, true), true);
    EXPECT_EQ(count(OS_EventType::KeyDown, OS_Key::W), 1u);
    like_sdls_pump(key_event(13, false), true);
    EXPECT_EQ(count(OS_EventType::KeyUp, OS_Key::W), 1u);
}

namespace
{

struct PumpProbe
{
    PlatformAppEventsTest* test;
    u32                    after_pump; // W presses queued right after os_pump_window_events
    b8                     stopped;
};

} // namespace

static u32 stop_app_calls = 0;

static void stop_app (void* app)
{
    stop_app_calls++;
    objc_send<void>(app, "stop:", (void*)nullptr);
    // stop: takes effect after the next event; an application-defined one wakes the loop
    void* const wake = objc_send<void*>(objc_getClass("NSEvent"), "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:",
                                        (u64)15, NSPointValue{ 0.0, 0.0 }, (u64)0, (f64)0.0, (i64)0, (void*)nullptr, (i16)0, (i64)0, (i64)0);
    objc_send<void>(app, "postEvent:atStart:", wake, (signed char)0);
}

// Posted here, not before the loop runs: the loop may dispatch an event posted earlier before it
// runs this, which made the app's own dispatch look like the pump's
static void pump_inside_the_loop (void* user)
{
    PumpProbe& probe = *(PumpProbe*)user;
    objc_send<void>(probe.test->app, "postEvent:atStart:", probe.test->key_event(13, true), (signed char)0);
    os_pump_window_events();
    probe.after_pump = probe.test->count(OS_EventType::KeyDown, OS_Key::W);
    stop_app(probe.test->app);
}

// The app whose event loop the safety stop ends; cleared once the loop returns, as the stop fires
// seconds later, in whatever test runs the main queue then
static void* safety_stop_app = nullptr;

static void safety_stop (void*)
{
    if (safety_stop_app)
    {
        stop_app(safety_stop_app);
        safety_stop_app = nullptr;
    }
}

static void run_the_event_loop (PumpProbe& probe, dispatch_function_t inside, i64 safety_stop_ns)
{
    dispatch_async_f(dispatch_get_main_queue(), &probe, inside);
    safety_stop_app = probe.test->app;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, safety_stop_ns), dispatch_get_main_queue(), nullptr, safety_stop);
    objc_send<void>(probe.test->app, "run");
    safety_stop_app = nullptr;
}

// Inside NSApp's running event loop (Unity's editor and players) the app dispatches the events;
// a pump there took them and ran the app's frames nested in puppetxr's tick
TEST_F(PlatformAppEventsTest, the_pump_leaves_events_to_the_app_running_its_event_loop)
{
    PumpProbe probe = { .test = this };
    run_the_event_loop(probe, pump_inside_the_loop, 5 * (i64)NSEC_PER_SEC);
    EXPECT_EQ(probe.after_pump, 0u) << "the pump took the app's event";
    os_serve_posted(0);
    // The app dispatched it after, as Unity does, and the monitor handed it to SDL
    EXPECT_EQ(count(OS_EventType::KeyDown, OS_Key::W), 1u);
}

static void stop_inside_the_loop (void* user)
{
    stop_app(((PumpProbe*)user)->test->app);
}

// The safety stop outlives the loop it guards and fires in whatever test runs the main queue then.
// It read its probe from a stack the next test had reused, and stopped the app when what it found
// there read as not stopped
TEST_F(PlatformAppEventsTest, a_safety_stop_after_its_loop_returned_stops_nothing)
{
    PumpProbe probe = { .test = this };
    run_the_event_loop(probe, stop_inside_the_loop, 300 * (i64)NSEC_PER_MSEC);
    stop_app_calls = 0;
    probe           = { .test = this }; // the next test's probe, where the stack is reused
    const u64 start = os_time_ns();
    while (os_time_ns() - start < 600 * NSEC_PER_MSEC)
    {
        os_serve_posted(50 * NSEC_PER_MSEC);
    }
    EXPECT_EQ(stop_app_calls, 0u) << "the safety stop stopped an app after its loop returned";
}

static void pump_in_a_run_loop_pass (void* user)
{
    PumpProbe& probe = *(PumpProbe*)user;
    os_pump_window_events();
    probe.after_pump = probe.test->count(OS_EventType::KeyDown, OS_Key::W);
    probe.stopped    = true;
    CFRunLoopStop(CFRunLoopGetMain());
}

// A run loop pass that is not the event loop (os_serve_posted, an app polling events itself)
// still pumps
TEST_F(PlatformAppEventsTest, the_pump_dispatches_events_in_a_run_loop_without_the_event_loop)
{
    PumpProbe probe = { .test = this };
    objc_send<void>(app, "postEvent:atStart:", key_event(13, true), (signed char)0);
    dispatch_async_f(dispatch_get_main_queue(), &probe, pump_in_a_run_loop_pass);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 5.0, 0);
    EXPECT_TRUE(probe.stopped);
    EXPECT_EQ(probe.after_pump, 1u);
}

TEST_F(PlatformAppEventsTest, mouse_moves_the_app_dispatches_turn_while_looking)
{
    os_window_show(window);
    os_window_set_relative_mouse(window, true);
    defer { os_window_set_relative_mouse(window, false); };
    OS_Event drain[64];
    os_poll_events(drain, 64);
    like_the_app(move_event(7, 3));
    EXPECT_EQ(count(OS_EventType::MouseMotion), 1u);
    like_sdls_pump(move_event(5, 2), false);
    EXPECT_EQ(count(OS_EventType::MouseMotion), 1u) << "a move SDL's pump dispatches turns once";
}
#endif

// SDL maximizes a hidden window with -[NSWindow zoom:] as it shows it, which animates for some
// 240 ms on the thread showing it: the first frame of an app waited for it
TEST(Platform, showing_a_maximized_window_does_not_wait_for_an_animation)
{
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    OS_Window* window = os_create_window("maximized", 320, 240, false, 0, true);
    ASSERT_NE(window, nullptr);
    defer { os_destroy_window(window); };
    if (!os_window_maximize(window))
    {
        GTEST_SKIP() << "the video driver cannot maximize";
    }
    const u64 start = os_monotonic_ns();
    os_window_show(window);
    // The animation takes some 370 ms; a show takes milliseconds, under ThreadSanitizer 70
    EXPECT_LT(os_monotonic_ns() - start, 150000000ull);
    u32 width  = 0;
    u32 height = 0;
    os_window_size(window, width, height);
    // On X11 the window manager maximizes, and an X server without one, like Xvfb, never does;
    // Wine then calls the window maximized at its old size
    if (on_x11() && (!os_window_maximized(window) || width == 320))
    {
        GTEST_SKIP() << "no window manager maximizes the window";
    }
    EXPECT_TRUE(os_window_maximized(window));
    EXPECT_GT(width, 320u);
    EXPECT_GT(height, 240u);
}

// A Metal app like Unity's macOS player loads no Vulkan loader: its mirror window still opens,
// without Vulkan, where SDL failed the window and the runtime went headless
TEST(Platform, a_window_without_vulkan_still_opens)
{
    os_shutdown();
    os_set_env("SDL_VULKAN_LIBRARY", "/nonexistent/libvulkan.1.dylib");
    defer {
        os_unset_env("SDL_VULKAN_LIBRARY");
        os_shutdown();
    };
    ASSERT_TRUE(os_initialize(false));
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    OS_Window* window = os_create_window("no vulkan", 64, 64, true, 0, true);
    EXPECT_NE(window, nullptr);
    os_destroy_window(window);
}

static void nothing_to_do (void* user)
{
    (void)user;
}

static i32 wake_the_window_thread (void* user)
{
    SDL_AtomicInt* stop = (SDL_AtomicInt*)user;
    while (!SDL_GetAtomicInt(stop))
    {
        os_post_to_window_thread(nothing_to_do, nullptr);
        SDL_DelayNS(20000);
    }
    return 0;
}

// Regression: SDL's wakeup takes the window the window thread waits on and then sends it an event
// over a second X connection. A window closed in between ended the process with X's BadWindow
TEST(Platform, windows_close_while_other_threads_wake_the_window_thread)
{
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    SDL_AtomicInt stop = {};
    OS_Thread*    wakers[2];
    for (OS_Thread*& waker : wakers)
    {
        waker = os_thread_create("waker", wake_the_window_thread, &stop);
    }
    for (u32 i = 0; i < 100; i++)
    {
        OS_Window* window = os_create_window("closing", 64, 64, true);
        ASSERT_NE(window, nullptr);
        os_serve_posted(1000000);
        os_destroy_window(window);
    }
    SDL_SetAtomicInt(&stop, 1);
    for (OS_Thread* waker : wakers)
    {
        os_thread_join(waker);
    }
}

static u32 count_text (const char* text, const char* needle)
{
    u32 count = 0;
    for (const char* at = cstr_find(text, needle); at; at = cstr_find(at + 1, needle))
    {
        count++;
    }
    return count;
}

static char* read_profile (const char* path)
{
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        return nullptr;
    }
    char* text = (char*)os_memory_alloc(size + 1);
    memory_copy(text, data, size);
    text[size] = 0;
    os_memory_release(data);
    return text;
}

static void nested_zones ()
{
    PROF_ZONE("outer");
    PROF_ZONE("inner");
}

// Zones stay compiled in, dist builds too, so stopped they must cost next to nothing
TEST(Profile, zones_cost_next_to_nothing_while_stopped)
{
    ASSERT_FALSE(os_profile_active());
    const u64 start = os_monotonic_ns();
    for (u32 i = 0; i < 1000000; i++)
    {
        nested_zones();
    }
    // Sanitizers make everything several times slower, as in tests_oxr.cpp (TEST_TIME_SCALE)
    u64 bound_ns = 50000000ull;
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
    bound_ns *= 8;
#endif
#endif
    EXPECT_LT(os_monotonic_ns() - start, bound_ns) << "two million stopped zones under 50 ms";
}

// A Chrome trace: complete events per zone, a name per thread; counted in the raw text, as a JSON
// parse of a big trace took most of a minute
TEST(Profile, a_capture_writes_the_zones_of_every_thread)
{
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_profile_test.json", temp_dir());
    ASSERT_TRUE(os_profile_start(path));
    EXPECT_TRUE(os_profile_active());
    EXPECT_FALSE(os_profile_start(path)) << "one capture at a time";
    for (u32 i = 0; i < 10; i++)
    {
        nested_zones();
    }
    OS_Thread* thread = os_thread_create("profiled", [](void*) -> i32 {
        for (u32 i = 0; i < 5; i++)
        {
            PROF_ZONE("on the thread");
        }
        return 0;
    }, nullptr);
    os_thread_join(thread);
    os_profile_stop();
    EXPECT_FALSE(os_profile_active());
    nested_zones();

    char* text = read_profile(path);
    ASSERT_NE(text, nullptr);
    defer { os_memory_release(text); };
    EXPECT_TRUE(cstr_starts_with(text, "{\"traceEvents\":["));
    EXPECT_EQ(count_text(text, "\"name\":\"outer\""), 10u);
    EXPECT_EQ(count_text(text, "\"name\":\"inner\""), 10u);
    EXPECT_EQ(count_text(text, "\"name\":\"on the thread\""), 5u);
    EXPECT_GE(count_text(text, "\"thread_name\""), 2u);
    EXPECT_NE(cstr_find(text, "\"name\":\"profiled\""), nullptr);
    os_delete_file(path);
}

// A zone open when a capture starts belongs to neither capture; zones deeper than the limit are
// not recorded, and the ones around them still are
TEST(Profile, open_and_too_deep_zones_are_left_out)
{
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_profile_depth.json", temp_dir());
    {
        PROF_ZONE("open before");
        ASSERT_TRUE(os_profile_start(path));
    }
    for (u32 depth = 0; depth < OS_PROFILE_MAX_DEPTH + 5; depth++)
    {
        os_profile_begin("deep");
    }
    for (u32 depth = 0; depth < OS_PROFILE_MAX_DEPTH + 5; depth++)
    {
        os_profile_end();
    }
    os_profile_stop();
    char* text = read_profile(path);
    ASSERT_NE(text, nullptr);
    defer { os_memory_release(text); };
    EXPECT_EQ(count_text(text, "\"name\":\"open before\""), 0u);
    EXPECT_EQ(count_text(text, "\"name\":\"deep\""), (u32)OS_PROFILE_MAX_DEPTH);
    os_delete_file(path);
}

// A stalled app polls nothing while the user goes on moving the mouse; W released then stayed held
// in the simulator, as the release came after more events than the window queue and one poll hold
TEST(Platform, a_release_after_a_flood_of_moves_is_not_lost)
{
    os_set_env("SDL_VIDEODRIVER", "offscreen");
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer {
        os_shutdown();
        os_unset_env("SDL_VIDEODRIVER");
    };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    OS_Window* window = os_create_window("test", 64, 64, false);
    ASSERT_NE(window, nullptr);
    defer { os_destroy_window(window); };
    OS_Event events[64];
    while (os_poll_events(events, 64) > 0)
    {
    }

    ASSERT_TRUE(os_push_mouse_event(window, 10.0f, 10.0f, OS_Key::MouseLeft, true));
    for (u32 i = 0; i < 2000; i++)
    {
        ASSERT_TRUE(os_push_mouse_event(window, (f32)(i % 50), (f32)(i % 40), OS_Key::None, false));
    }
    ASSERT_TRUE(os_push_mouse_event(window, 10.0f, 10.0f, OS_Key::MouseLeft, false));

    b8 down = false;
    b8 up   = false;
    for (u32 poll = 0; poll < 100 && !up; poll++)
    {
        const u32 count = os_poll_events(events, 64);
        for (u32 i = 0; i < count; i++)
        {
            down = down || (events[i].type == OS_EventType::KeyDown && events[i].key == OS_Key::MouseLeft);
            up   = up || (events[i].type == OS_EventType::KeyUp && events[i].key == OS_Key::MouseLeft);
        }
    }
    EXPECT_TRUE(down);
    EXPECT_TRUE(up);

    // A key held through the stall repeats some 30 times a second
    ASSERT_TRUE(os_push_key_event(window, OS_Key::W, true, false));
    for (u32 i = 0; i < 2000; i++)
    {
        ASSERT_TRUE(os_push_key_event(window, OS_Key::W, true, true));
    }
    ASSERT_TRUE(os_push_key_event(window, OS_Key::W, false, false));
    b8 released = false;
    for (u32 poll = 0; poll < 100 && !released; poll++)
    {
        const u32 count = os_poll_events(events, 64);
        for (u32 i = 0; i < count; i++)
        {
            released = released || (events[i].type == OS_EventType::KeyUp && events[i].key == OS_Key::W);
        }
    }
    EXPECT_TRUE(released);
    EXPECT_FALSE(os_push_key_event(window, OS_Key::None, true, false));
}

TEST(Platform, windows_start_hidden_until_shown)
{
    os_set_env("SDL_VIDEODRIVER", "offscreen");
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer {
        os_shutdown();
        os_unset_env("SDL_VIDEODRIVER");
    };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }

    OS_Window* shown = os_create_window("test", 64, 64, false);
    ASSERT_NE(shown, nullptr);
    EXPECT_TRUE(os_window_shown(shown));
    os_destroy_window(shown);

    OS_Window* hidden = os_create_window("test", 64, 64, false, 0, true);
    ASSERT_NE(hidden, nullptr);
    defer { os_destroy_window(hidden); };
    EXPECT_FALSE(os_window_shown(hidden));
    os_window_show(hidden);
    EXPECT_TRUE(os_window_shown(hidden));
    os_window_hide(hidden);
    EXPECT_FALSE(os_window_shown(hidden));
    os_window_show(hidden);
    EXPECT_TRUE(os_window_shown(hidden));
    EXPECT_FALSE(os_window_shown(nullptr));
    os_window_show(nullptr);
    os_window_hide(nullptr);
    f32 x = 1.0f;
    f32 y = 1.0f;
    EXPECT_FALSE(os_window_cursor(nullptr, x, y));
    EXPECT_FALSE(os_window_cursor(hidden, x, y)) << "an offscreen window has no cursor over it";
}

// The UI takes the cursor each frame, as Windows may keep a mouse's moves from the window; a real
// cursor resting over the window then outranked the moves and clicks a test or puppetxrctl pushed,
// and a pushed press landed on a panel instead of the view
TEST(Platform, pushed_mouse_events_outrank_the_real_cursor)
{
#if defined(_WIN32)
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer { os_shutdown(); };
    if (os_is_headless() || cstr_equal(os_video_driver(), "offscreen"))
    {
        GTEST_SKIP() << "no window driver";
    }
    OS_Window* window = os_create_window("test", 320, 200, false);
    ASSERT_NE(window, nullptr);
    defer { os_destroy_window(window); };
    OS_WindowNative native = {};
    ASSERT_TRUE(os_window_native_handles(window, native));
    POINT saved = {};
    ASSERT_TRUE(GetCursorPos(&saved));
    defer { SetCursorPos(saved.x, saved.y); };
    POINT inside = { 160, 100 };
    ASSERT_TRUE(ClientToScreen((HWND)native.window, &inside));
    ASSERT_TRUE(SetCursorPos(inside.x, inside.y));
    OS_Event drain[64];
    os_poll_events(drain, 64);
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (!os_window_cursor(window, x, y))
    {
        GTEST_SKIP() << "another window is over the test window";
    }
    EXPECT_EQ(x, 160.0f);

    ASSERT_TRUE(os_push_mouse_event(window, 10.0f, 10.0f, OS_Key::None, false));
    os_poll_events(drain, 64);
    EXPECT_FALSE(os_window_cursor(window, x, y)) << "pushed events drive the mouse";
#else
    GTEST_SKIP() << "the cursor is read on Windows only";
#endif
}

TEST(Platform, windows_maximize)
{
    os_set_env("SDL_VIDEODRIVER", "offscreen");
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer {
        os_shutdown();
        os_unset_env("SDL_VIDEODRIVER");
    };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }

    OS_Window* window = os_create_window("test", 320, 200, false);
    ASSERT_NE(window, nullptr);
    defer { os_destroy_window(window); };
    EXPECT_FALSE(os_window_maximized(window));
    EXPECT_FALSE(os_window_maximize(nullptr));
    EXPECT_FALSE(os_window_maximized(nullptr));

    // SDL's offscreen driver cannot maximize; it must say so instead of pretending
    const b8 supported = os_window_maximize(window);
    OS_Event drain[64];
    os_poll_events(drain, 64);
    EXPECT_EQ(os_window_maximized(window), supported);
    EXPECT_EQ(supported, !cstr_equal(os_video_driver(), "offscreen"));
}

// GL mirror windows need the native window of the driver the app's display is on
TEST(Platform, video_driver_switches_and_windows_have_native_handles)
{
    os_set_env("SDL_VIDEODRIVER", "offscreen");
    os_shutdown();
    ASSERT_TRUE(os_initialize(false));
    defer {
        os_shutdown();
        os_unset_env("SDL_VIDEODRIVER");
    };
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    ASSERT_NE(os_video_driver(), nullptr);
    EXPECT_STREQ(os_video_driver(), "offscreen");

    OS_Window*      window = os_create_window("test", 64, 64, false);
    OS_WindowNative native = {};
    ASSERT_NE(window, nullptr);
    EXPECT_FALSE(os_window_native_handles(window, native)) << "an offscreen window has no native window";
    EXPECT_FALSE(os_window_native_handles(nullptr, native));
    os_destroy_window(window);

    EXPECT_TRUE(os_set_video_driver("offscreen"));
    EXPECT_FALSE(os_set_video_driver("puppetxr_no_such_driver"));
    EXPECT_FALSE(os_set_video_driver(nullptr));
    EXPECT_STREQ(os_video_driver(), "offscreen") << "a failed switch keeps the driver";

    if (!os_env("DISPLAY") || !os_set_video_driver("x11"))
    {
        return;
    }
    EXPECT_STREQ(os_video_driver(), "x11");
    window = os_create_window("test", 64, 64, false);
    ASSERT_NE(window, nullptr);
    ASSERT_TRUE(os_window_native_handles(window, native));
    EXPECT_NE(native.window, 0u);
    EXPECT_NE(native.display, nullptr);
    EXPECT_FALSE(native.wayland);
    os_destroy_window(window);
}

namespace
{

struct LogSink
{
    u32         count;
    OS_LogLevel level;
    char        text[OS_LOG_LINE_LENGTH];
    b8          log_from_callback;
};

} // namespace

static void log_sink (OS_LogLevel level, const char* text, void* user)
{
    LogSink& sink = *(LogSink*)user;
    sink.count++;
    sink.level = level;
    cstr_copy(sink.text, sizeof(sink.text), text);
    if (sink.log_from_callback)
    {
        LOG_INFO("logged from the callback");
    }
}

TEST(Platform, log_callback_receives_runtime_lines)
{
    LogSink sink = {};
    os_set_log_callback(log_sink, &sink);
    LOG_WARNING("callback %d", 7);
    EXPECT_EQ(sink.count, 1u);
    EXPECT_EQ(sink.level, OS_LogLevel::Warning);
    EXPECT_TRUE(cstr_equal(sink.text, "callback 7"));

    os_set_log_level(OS_LogLevel::Error);
    LOG_INFO("filtered");
    os_set_log_level(OS_LogLevel::Info);
    EXPECT_EQ(sink.count, 1u);

    // A callback that logs gets its own line into the history, not a second call
    sink.log_from_callback = true;
    LOG_INFO("outer");
    EXPECT_EQ(sink.count, 2u);
    EXPECT_TRUE(cstr_equal(sink.text, "outer"));
    EXPECT_TRUE(cstr_equal(os_log_history(os_log_history_count() - 1)->text, "logged from the callback"));

    os_set_log_callback(nullptr, nullptr);
    LOG_INFO("after removal");
    EXPECT_EQ(sink.count, 2u);
}

namespace
{
struct ClaimRacer
{
    u32* flag;
    b8   claimed;
};
} // namespace

static i32 claim_race (void* user)
{
    ClaimRacer* racer = (ClaimRacer*)user;
    racer->claimed    = os_claim_once(racer->flag);
    return 0;
}

TEST(Platform, claim_once_is_true_for_the_first_caller_only)
{
    u32 flag = 0;
    EXPECT_TRUE(os_claim_once(&flag));
    EXPECT_FALSE(os_claim_once(&flag));
    EXPECT_FALSE(os_claim_once(&flag));

    // Threads racing for one flag: exactly one wins every round
    for (u32 round = 0; round < 50; round++)
    {
        u32        shared     = 0;
        ClaimRacer racers[4]  = {};
        OS_Thread* threads[4] = {};
        for (u32 i = 0; i < 4; i++)
        {
            racers[i]  = { .flag = &shared };
            threads[i] = os_thread_create("claim", claim_race, &racers[i]);
        }
        u32 winners = 0;
        for (u32 i = 0; i < 4; i++)
        {
            os_thread_join(threads[i]);
            winners += racers[i].claimed ? 1u : 0u;
        }
        EXPECT_EQ(winners, 1u);
    }
}

TEST(Platform, log_history_keeps_the_latest_lines)
{
    const u64 before = os_log_sequence();
    LOG_INFO("history test %d", 1);
    LOG_WARNING("history test %d", 2);
    EXPECT_EQ(os_log_sequence(), before + 2);
    const u32 count = os_log_history_count();
    ASSERT_GE(count, 2u);
    const OS_LogLine* last = os_log_history(count - 1);
    ASSERT_NE(last, nullptr);
    EXPECT_TRUE(cstr_equal(last->text, "history test 2"));
    EXPECT_EQ(last->level, OS_LogLevel::Warning);
    EXPECT_TRUE(cstr_equal(os_log_history(count - 2)->text, "history test 1"));
    EXPECT_EQ(os_log_history(count), nullptr);

    for (u32 i = 0; i < OS_LOG_HISTORY + 5; i++)
    {
        LOG_INFO("flood %u", i);
    }
    EXPECT_EQ(os_log_history_count(), (u32)OS_LOG_HISTORY);
    EXPECT_TRUE(cstr_equal(os_log_history(OS_LOG_HISTORY - 1)->text, "flood 516"));
    EXPECT_TRUE(cstr_equal(os_log_history(0)->text, "flood 5"));
}

TEST(Platform, app_output_is_captured_into_the_log)
{
    if (!os_capture_output_begin())
    {
        GTEST_SKIP() << "output capture not supported here";
    }
    printf("captured line one\n");
    (void)fprintf(stderr, "captured line two\n");
    (void)fflush(stdout);
    (void)fflush(stderr);
    LOG_INFO("own line");
    os_capture_output_poll();
    os_capture_output_end();

    const u32 count = os_log_history_count();
    ASSERT_GE(count, 3u);
    b8 saw_one = false;
    b8 saw_two = false;
    b8 saw_own = false;
    for (u32 i = 0; i < count; i++)
    {
        const OS_LogLine* line = os_log_history(i);
        saw_one |= line->source == OS_LogSource::App && cstr_equal(line->text, "captured line one");
        saw_two |= line->source == OS_LogSource::App && cstr_equal(line->text, "captured line two");
        saw_own |= line->source == OS_LogSource::Sim && cstr_equal(line->text, "own line");
    }
    EXPECT_TRUE(saw_one);
    EXPECT_TRUE(saw_two);
    EXPECT_TRUE(saw_own);
    printf("stdout works again\n");
}

TEST(Platform, streaming_file_writes)
{
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_test_stream.bin", temp_dir());
    OS_File* file = os_file_create(path);
    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(os_file_write(file, "abc", 3));
    EXPECT_TRUE(os_file_write(file, "def", 3));
    EXPECT_TRUE(os_file_write(file, nullptr, 0));
    EXPECT_TRUE(os_file_close(file));

    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    EXPECT_EQ(size, 6u);
    EXPECT_TRUE(memory_equal(data, "abcdef", 6));
    os_memory_release(data);

    EXPECT_EQ(os_file_create("/nonexistent/dir/file.bin"), nullptr);
    EXPECT_EQ(os_file_create(nullptr), nullptr);
    EXPECT_FALSE(os_file_write(nullptr, "a", 1));
    EXPECT_FALSE(os_file_close(nullptr));
}

// PUPPETXR_LOG_FILE: a test driver reads the log while the app writes it, and two processes may
// append to one log; on Windows the file was opened for this process alone
TEST(Platform, appended_files_are_shared_while_open)
{
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_test_append_%u.log", temp_dir(), os_process_id());
    defer { os_delete_file(path); };
    ASSERT_TRUE(os_write_file(path, "a", 1));
    OS_File* first = os_file_append(path);
    ASSERT_NE(first, nullptr);
    OS_File* second = os_file_append(path);
    ASSERT_NE(second, nullptr) << "a second writer";
    EXPECT_TRUE(os_file_write(first, "b", 1));
    EXPECT_TRUE(os_file_flush(first));
    EXPECT_TRUE(os_file_write(second, "c", 1));
    EXPECT_TRUE(os_file_flush(second));

    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size)) << "read while both are open";
    EXPECT_EQ(size, 3u);
    EXPECT_TRUE(memory_equal(data, "abc", 3));
    os_memory_release(data);
    EXPECT_TRUE(os_file_close(first));
    EXPECT_TRUE(os_file_close(second));
    EXPECT_EQ(os_file_append(nullptr), nullptr);
}

TEST(Platform, atomic_writes_replace_the_whole_file_and_nested_directories_are_created)
{
    char top[OS_MAX_PATH];
    char dir[OS_MAX_PATH];
    cstr_format(top, sizeof(top), "%s/puppetxr_test_atomic_%u", temp_dir(), os_process_id());
    cstr_format(dir, sizeof(dir), "%s/a/b", top);
    defer { os_delete_tree(top); };
    ASSERT_TRUE(os_create_directories(dir));
    EXPECT_TRUE(os_directory_exists(dir));
    EXPECT_TRUE(os_create_directories(dir)) << "existing directories are fine";

    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/store.json", dir);
    ASSERT_TRUE(os_write_file_atomic(path, "first version", 13));
    ASSERT_TRUE(os_write_file_atomic(path, "second", 6));
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    EXPECT_EQ(size, 6u);
    EXPECT_TRUE(memory_equal(data, "second", 6));
    os_memory_release(data);

    char temporary[OS_MAX_PATH];
    cstr_format(temporary, sizeof(temporary), "%s.tmp", path);
    EXPECT_FALSE(os_file_exists(temporary));
    EXPECT_FALSE(os_write_file_atomic("/nonexistent/dir/file", "x", 1));
    EXPECT_FALSE(os_create_directories(nullptr));
    EXPECT_TRUE(os_delete_file(path));
}

TEST(Platform, log_file_from_environment)
{
    // The file is opened on the first log line, once until os_release closes it
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_test_log.txt", temp_dir());
    (void)remove(path);
    os_set_env("PUPPETXR_LOG_FILE", path);
    os_release();
    const u64 before = os_log_sequence();
    LOG_INFO("log file check");
    EXPECT_EQ(os_log_sequence(), before + 1);
    // Regression: on Windows a running app held its log for itself, and a driver reading it failed
    u8*   data = nullptr;
    usize size = 0;
    EXPECT_TRUE(os_read_file(path, &data, &size)) << "read while open";
    os_memory_release(data);
    data = nullptr;
    os_release();
    os_unset_env("PUPPETXR_LOG_FILE");

    ASSERT_TRUE(os_read_file(path, &data, &size));
    char text[4096] = {};
    memory_copy(text, data, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
    os_memory_release(data);
    EXPECT_NE(cstr_find(text, "log file check"), nullptr);
    (void)remove(path);
}

// Regression: the log mutex and log file outlived the runtime when the loader unloaded it
TEST(Platform, release_frees_the_log_state_and_logging_recovers)
{
    LOG_INFO("before release");
    os_release();
    os_release();
    const u64 before = os_log_sequence();
    LOG_INFO("after release");
    EXPECT_EQ(os_log_sequence(), before + 1);
    EXPECT_TRUE(cstr_equal(os_log_history(os_log_history_count() - 1)->text, "after release"));
}

static OS_Socket* accept_within (const OS_Socket* listener, u32 timeout_ms)
{
    for (u32 waited = 0; waited < timeout_ms; waited += 5)
    {
        if (OS_Socket* client = os_socket_accept(listener))
        {
            return client;
        }
        os_sleep_ns(5000000);
    }
    return nullptr;
}

TEST(Platform, sockets_connect_send_and_receive)
{
    OS_Socket* listener = os_socket_listen("127.0.0.1", 0);
    ASSERT_NE(listener, nullptr);
    const u16 port = os_socket_port(listener);
    EXPECT_NE(port, 0);
    EXPECT_EQ(os_socket_accept(listener), nullptr);

    OS_Socket* client = os_socket_connect("127.0.0.1", port, 1000);
    ASSERT_NE(client, nullptr);
    OS_Socket* server = accept_within(listener, 1000);
    ASSERT_NE(server, nullptr);

    usize sent = 0;
    EXPECT_EQ(os_socket_send(client, "hello\n", 6, sent), OS_SocketResult::Ok);
    EXPECT_EQ(sent, 6u);
    ASSERT_TRUE(os_socket_wait(server, 1000));
    char  buffer[16] = {};
    usize received   = 0;
    EXPECT_EQ(os_socket_receive(server, buffer, sizeof(buffer), received), OS_SocketResult::Ok);
    EXPECT_EQ(received, 6u);
    EXPECT_TRUE(memory_equal(buffer, "hello\n", 6));
    EXPECT_EQ(os_socket_receive(server, buffer, sizeof(buffer), received), OS_SocketResult::WouldBlock);

    os_socket_close(client);
    ASSERT_TRUE(os_socket_wait(server, 1000));
    EXPECT_EQ(os_socket_receive(server, buffer, sizeof(buffer), received), OS_SocketResult::Closed);
    // Sending to a closed peer must report it, not raise SIGPIPE
    for (u32 i = 0; i < 4 && os_socket_send(server, "x", 1, sent) == OS_SocketResult::Ok; i++)
    {
        os_sleep_ns(5000000);
    }
    EXPECT_EQ(os_socket_send(server, "x", 1, sent), OS_SocketResult::Closed);
    os_socket_close(server);

    os_socket_close(listener);
    EXPECT_EQ(os_socket_connect("127.0.0.1", port, 200), nullptr);
    EXPECT_EQ(os_socket_listen("not an address", 0), nullptr);
}

// Saves an environment variable and puts it back at the end of the scope
namespace
{

struct SavedEnv
{
    const char* name;
    b8          set;
    char        value[OS_MAX_PATH];

    explicit SavedEnv (const char* variable) : name(variable), set(false), value()
    {
        const char* current = os_env(name);
        set                 = current != nullptr;
        cstr_copy(value, sizeof(value), current ? current : "");
    }

    ~SavedEnv ()
    {
        set ? os_set_env(name, value) : os_unset_env(name);
    }

    SavedEnv (const SavedEnv&)            = delete;
    SavedEnv& operator= (const SavedEnv&) = delete;
};

} // namespace

TEST(Platform, absolute_paths_are_recognized)
{
    EXPECT_TRUE(os_path_is_absolute("/tmp/a"));
    EXPECT_FALSE(os_path_is_absolute("a/b"));
    EXPECT_FALSE(os_path_is_absolute("./a"));
    EXPECT_FALSE(os_path_is_absolute(""));
    EXPECT_FALSE(os_path_is_absolute(nullptr));
#if HAVE_WINDOWS
    EXPECT_TRUE(os_path_is_absolute("D:\\dev\\a.png"));
    EXPECT_TRUE(os_path_is_absolute("c:/dev/a.png"));
    EXPECT_TRUE(os_path_is_absolute("\\\\server\\share\\a.png"));
    EXPECT_FALSE(os_path_is_absolute("D:a.png")) << "relative to the drive's current directory";
#else
    EXPECT_FALSE(os_path_is_absolute("D:\\dev\\a.png"));
#endif
}

TEST(Platform, temp_directories_are_inside_the_temp_dir)
{
    char base[OS_MAX_PATH];
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_temp_dir(base, sizeof(base)));
    ASSERT_TRUE(os_create_temp_directory("puppetxr_temp_test", dir, sizeof(dir)));
    const usize length = cstr_length(base);
    EXPECT_TRUE(cstr_starts_with(dir, base)) << dir;
    EXPECT_TRUE(dir[length] == '/' || dir[length] == '\\') << dir;
    EXPECT_TRUE(os_directory_exists(dir)) << dir;
    EXPECT_TRUE(os_delete_tree(dir));
}

// puppetxrctl gives up on a target whose process exited, whose discovery file may still be there
TEST(Platform, a_process_is_alive_while_it_runs)
{
    EXPECT_TRUE(os_process_alive(os_process_id()));
    EXPECT_FALSE(os_process_alive(0));
    EXPECT_FALSE(os_process_alive(0xFFFFFFF0u));
}

// Tests delete the temp directories they made, or thousands pile up in the temp dir
TEST(Platform, a_tree_is_deleted_with_everything_in_it)
{
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_create_temp_directory("puppetxr_tree_test", dir, sizeof(dir)));
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/a/b", dir);
    ASSERT_TRUE(os_create_directories(path));
    cstr_format(path, sizeof(path), "%s/a/b/file.txt", dir);
    ASSERT_TRUE(os_write_file(path, "x", 1));
    cstr_format(path, sizeof(path), "%s/top.txt", dir);
    ASSERT_TRUE(os_write_file(path, "y", 1));
    EXPECT_TRUE(os_delete_tree(dir));
    EXPECT_FALSE(os_directory_exists(dir));
    EXPECT_TRUE(os_delete_tree(dir)) << "nothing left to delete";
}

TEST(Platform, state_dir_can_be_forced)
{
    const SavedEnv forced("PUPPETXR_STATE_DIR");
    char           base[OS_MAX_PATH];
    cstr_format(base, sizeof(base), "%s/puppetxr_forced_state_%u/a", PUPPETXR_TEST_OUTPUT_DIR, os_process_id());
    os_set_env("PUPPETXR_STATE_DIR", base);
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_state_dir(dir, sizeof(dir)));
    EXPECT_STREQ(dir, base);
    EXPECT_TRUE(os_directory_exists(dir)) << "created with its parents";
    EXPECT_FALSE(os_state_dir(dir, 4));
}

#if HAVE_WINDOWS
TEST(Platform, state_dir_is_in_local_app_data)
{
    const SavedEnv forced("PUPPETXR_STATE_DIR");
    const SavedEnv local("LOCALAPPDATA");
    os_unset_env("PUPPETXR_STATE_DIR");

    char base[OS_MAX_PATH];
    cstr_format(base, sizeof(base), "%s/puppetxr_local_app_data_%u", PUPPETXR_TEST_OUTPUT_DIR, os_process_id());
    ASSERT_TRUE(os_create_directory(base));
    os_set_env("LOCALAPPDATA", base);
    char dir[OS_MAX_PATH];
    char expected[OS_MAX_PATH];
    ASSERT_TRUE(os_state_dir(dir, sizeof(dir)));
    cstr_format(expected, sizeof(expected), "%s\\puppetxr", base);
    EXPECT_STREQ(dir, expected);
    EXPECT_TRUE(os_directory_exists(dir));

    os_unset_env("LOCALAPPDATA");
    EXPECT_FALSE(os_state_dir(dir, sizeof(dir)));
}
#else
TEST(Platform, state_dir_follows_xdg_and_falls_back_to_home)
{
    const SavedEnv forced("PUPPETXR_STATE_DIR");
    const SavedEnv xdg("XDG_STATE_HOME");
    const SavedEnv home("HOME");
    os_unset_env("PUPPETXR_STATE_DIR");

    char base[OS_MAX_PATH];
    cstr_format(base, sizeof(base), "%s/puppetxr_state_dir_%u", PUPPETXR_TEST_OUTPUT_DIR, os_process_id());
    ASSERT_TRUE(os_create_directory(base));

    char dir[OS_MAX_PATH];
    char expected[OS_MAX_PATH];
    os_set_env("XDG_STATE_HOME", base);
    ASSERT_TRUE(os_state_dir(dir, sizeof(dir)));
    cstr_format(expected, sizeof(expected), "%s/puppetxr", base);
    EXPECT_STREQ(dir, expected);
    EXPECT_TRUE(os_directory_exists(dir));

    // The spec says a relative XDG path is invalid and must be ignored
    os_set_env("XDG_STATE_HOME", "relative/dir");
    os_set_env("HOME", base);
    ASSERT_TRUE(os_state_dir(dir, sizeof(dir)));
    cstr_format(expected, sizeof(expected), "%s/.local/state/puppetxr", base);
    EXPECT_STREQ(dir, expected);
    EXPECT_TRUE(os_directory_exists(dir));

    os_unset_env("XDG_STATE_HOME");
    os_unset_env("HOME");
    EXPECT_FALSE(os_state_dir(dir, sizeof(dir)));
    EXPECT_FALSE(os_state_dir(dir, 4));
}
#endif

// The child of lock_files_are_stale_once_their_process_is_gone: takes the lock and ends
// without releasing it. A no-op in a normal run.
TEST(Platform, lock_file_holder_process)
{
    const char* path = os_env("PUPPETXR_TEST_LOCK_PATH");
    if (path && path[0])
    {
        os_lock_file_create(path, "{}", 2);
        // Dies like a crash: no atexit handlers, so LeakSanitizer does not fail the exit code
        _Exit(0);
    }
}

TEST(Platform, lock_files_are_stale_once_their_process_is_gone)
{
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_runtime_dir(dir, sizeof(dir)));
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_test_lock_%d.json", dir, (int)os_process_id());

    OS_LockFile* lock = os_lock_file_create(path, "{}", 2);
    ASSERT_NE(lock, nullptr);
    EXPECT_TRUE(os_file_exists(path));
    EXPECT_FALSE(os_lock_file_is_stale(path));
    os_lock_file_release(lock);
    EXPECT_FALSE(os_file_exists(path));

    ASSERT_TRUE(os_write_file(path, "{}", 2));
    EXPECT_TRUE(os_lock_file_is_stale(path));
    EXPECT_TRUE(os_delete_file(path));

    // A process that dies without cleaning up leaves a file nobody holds
    const SavedEnv    lock_path("PUPPETXR_TEST_LOCK_PATH");
    const char* const argv[] = { PUPPETXR_TESTS_PATH, "--gtest_filter=Platform.lock_file_holder_process", nullptr };
    char              log[OS_MAX_PATH];
    cstr_format(log, sizeof(log), "%s/puppetxr_lock_holder_%u.txt", temp_dir(), os_process_id());
    os_set_env("PUPPETXR_TEST_LOCK_PATH", path);
    OS_Process* child = os_process_spawn(argv, nullptr, log);
    os_unset_env("PUPPETXR_TEST_LOCK_PATH");
    ASSERT_NE(child, nullptr);
    i32 exit_code = -1;
    EXPECT_TRUE(os_process_wait(child, 30000, exit_code));
    os_process_release(child);
    EXPECT_EQ(exit_code, 0);
    EXPECT_TRUE(os_file_exists(path));
    EXPECT_TRUE(os_lock_file_is_stale(path));
    EXPECT_TRUE(os_delete_file(path));
    EXPECT_FALSE(os_lock_file_is_stale(path));
}

#if HAVE_WINDOWS
namespace
{
struct LockSweeper
{
    const char* temporary;
    OS_Mutex*   mutex;
    b8          stop;
};
} // namespace

// Sweeps the temporary file as the control server and puppetxrctl sweep stale files
static i32 sweep_temporary (void* user)
{
    LockSweeper& sweeper = *(LockSweeper*)user;
    for (;;)
    {
        os_mutex_lock(sweeper.mutex);
        const b8 stop = sweeper.stop;
        os_mutex_unlock(sweeper.mutex);
        if (stop)
        {
            return 0;
        }
        if (os_lock_file_is_stale(sweeper.temporary))
        {
            os_delete_file(sweeper.temporary);
        }
    }
}

// Simulators started side by side sweep each other's directory: a sweep must not delete the
// temporary file of a lock file still being written. On POSIX open and flock leave a short window.
TEST(Platform, lock_file_creation_survives_a_concurrent_sweep)
{
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_runtime_dir(dir, sizeof(dir)));
    char path[OS_MAX_PATH];
    char temporary[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_test_sweep_%u.json", dir, os_process_id());
    cstr_format(temporary, sizeof(temporary), "%s.%u.tmp", path, os_process_id());
    static const char data[64 * 1024] = {};

    LockSweeper sweeper = { .temporary = temporary, .mutex = os_mutex_create(), .stop = false };
    OS_Thread*  thread  = os_thread_create("sweeper", sweep_temporary, &sweeper);
    ASSERT_NE(thread, nullptr);
    u32 created = 0;
    for (u32 i = 0; i < 200; i++)
    {
        OS_LockFile* lock = os_lock_file_create(path, data, sizeof(data));
        created += lock ? 1 : 0;
        os_lock_file_release(lock);
    }
    os_mutex_lock(sweeper.mutex);
    sweeper.stop = true;
    os_mutex_unlock(sweeper.mutex);
    os_thread_join(thread);
    os_mutex_destroy(sweeper.mutex);
    EXPECT_EQ(created, 200u);
    EXPECT_FALSE(os_file_exists(path));
}

// Reads the lock file as puppetxrctl list and test drivers do: fopen shares neither delete nor a
// rename onto the file
static i32 read_lock_file (void* user)
{
    LockSweeper& reader = *(LockSweeper*)user;
    for (;;)
    {
        os_mutex_lock(reader.mutex);
        const b8 stop = reader.stop;
        os_mutex_unlock(reader.mutex);
        if (stop)
        {
            return 0;
        }
        if (FILE* file = fopen(reader.temporary, "rb"))
        {
            char buffer[16];
            fread(buffer, 1, sizeof(buffer), file);
            fclose(file);
        }
    }
}

// Unity creates instances one after the other in a process: the next control server writes its
// discovery file where the last one was deleted, and a reader holding that file made the rename
// fail with error 5, leaving the app's simulator without a discovery file
TEST(Platform, lock_file_creation_survives_a_concurrent_reader)
{
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_runtime_dir(dir, sizeof(dir)));
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_test_reader_%u.json", dir, os_process_id());
    static const char data[] = "{\"pid\":1}";

    LockSweeper reader = { .temporary = path, .mutex = os_mutex_create(), .stop = false };
    OS_Thread*  thread = os_thread_create("reader", read_lock_file, &reader);
    ASSERT_NE(thread, nullptr);
    u32 created = 0;
    for (u32 i = 0; i < 200; i++)
    {
        OS_LockFile* lock = os_lock_file_create(path, data, sizeof(data) - 1);
        created += lock ? 1 : 0;
        os_lock_file_release(lock);
    }
    os_mutex_lock(reader.mutex);
    reader.stop = true;
    os_mutex_unlock(reader.mutex);
    os_thread_join(thread);
    os_mutex_destroy(reader.mutex);
    EXPECT_EQ(created, 200u);
}
#endif

