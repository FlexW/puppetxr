#include "platform_internal.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <imm.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

//
// Types
//

#define OS_ENV_CACHE 64

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

// Native rather than the C runtime's, which shares nothing with readers while it writes
struct OS_File
{
    HANDLE handle;
    b8     failed;
};

// Recursive, as every OS_Mutex is
struct OS_Mutex
{
    CRITICAL_SECTION section;
};

struct OS_Thread
{
    HANDLE             handle;
    OS_ThreadFunction* function;
    void*              user;
    i32                result;
    wchar_t            name[64];
};

struct OS_Process
{
    HANDLE process;
};

namespace
{

// os_env answers with strings that stay valid until the variable changes
struct EnvEntry
{
    char  name[64];
    char* value;
};

} // namespace

//
// Statics
//

static SRWLOCK  os_env_lock                = SRWLOCK_INIT;
static EnvEntry os_env_cache[OS_ENV_CACHE] = {};
static HANDLE   os_wake_event              = nullptr;
static i64      os_ticks_per_second        = 0;

static HMODULE this_module ()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void*)&this_module, &module);
    return module;
}

// Paths and environment values are UTF-8 everywhere in puppetxr; Windows takes UTF-16
b8 os_win_to_wide (const char* text, wchar_t* out, usize capacity)
{
    if (!text || capacity == 0)
    {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, (int)capacity) > 0;
}

b8 os_win_from_wide (const wchar_t* text, char* out, usize capacity)
{
    if (!text || capacity == 0)
    {
        return false;
    }
    return WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)capacity, nullptr, nullptr) > 0;
}

static DWORD WINAPI thread_trampoline (void* user)
{
    OS_Thread* thread = (OS_Thread*)user;
    typedef HRESULT (WINAPI* SetThreadDescriptionFn)(HANDLE, PCWSTR);
    const SetThreadDescriptionFn set = (SetThreadDescriptionFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (set && thread->name[0])
    {
        set(GetCurrentThread(), thread->name);
    }
    thread->result = thread->function(thread->user);
    return 0;
}

//
// Memory
//

void* os_memory_alloc (usize size)
{
    return calloc(1, size ? size : 1);
}

void* os_memory_realloc (void* pointer, usize size)
{
    return realloc(pointer, size ? size : 1);
}

void os_memory_release (void* pointer)
{
    free(pointer);
}

//
// Time and environment
//

static i64 native_ticks_per_second ()
{
    if (os_ticks_per_second == 0)
    {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        os_ticks_per_second = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    }
    return os_ticks_per_second;
}

u64 os_monotonic_ns ()
{
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return os_native_ticks_to_ns(now.QuadPart);
}

u64 os_timespec_ns ()
{
    // 100 ns intervals since 1601, the same source as the UCRT's timespec_get(TIME_UTC)
    FILETIME now = {};
    GetSystemTimePreciseAsFileTime(&now);
    const u64 intervals = ((u64)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return (intervals - 116444736000000000ull) * 100;
}

OS_NativeClock os_native_clock ()
{
    return OS_NativeClock::PerformanceCounter;
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

// A high resolution waitable timer sleeps to the microsecond where Sleep rounds to the scheduler's
// tick; what the timer leaves is spun
void os_sleep_ns (u64 ns)
{
    const u64 end   = os_monotonic_ns() + ns;
    HANDLE    timer = ns > 1000000ull ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS) : nullptr;
    if (timer)
    {
        LARGE_INTEGER due = {};
        due.QuadPart      = -(LONGLONG)((ns - 500000ull) / 100ull);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
        }
        CloseHandle(timer);
    }
    else if (ns > 2000000ull)
    {
        Sleep((DWORD)((ns - 1000000ull) / 1000000ull));
    }
    while (os_monotonic_ns() < end)
    {
        YieldProcessor();
    }
}

// The C runtime's getenv answers in the ANSI code page and sees only its own copy. The process
// environment is read in UTF-16 instead and the UTF-8 value kept, valid until the variable changes
const char* os_env (const char* name)
{
    wchar_t wide_name[256];
    if (!name || cstr_length(name) >= sizeof(os_env_cache[0].name) || !os_win_to_wide(name, wide_name, ARRAY_SIZE(wide_name)))
    {
        return nullptr;
    }
    wchar_t wide_value[OS_MAX_PATH];
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableW(wide_name, wide_value, ARRAY_SIZE(wide_value));
    if (length >= ARRAY_SIZE(wide_value))
    {
        LOG_ERROR("Environment variable %s is longer than %u characters", name, (u32)ARRAY_SIZE(wide_value));
        return nullptr;
    }
    const b8 missing = length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND;
    char     value[OS_MAX_PATH * 3];
    wide_value[length] = 0;
    if (!missing && !os_win_from_wide(wide_value, value, sizeof(value)))
    {
        return nullptr;
    }
    AcquireSRWLockExclusive(&os_env_lock);
    EnvEntry* entry = nullptr;
    EnvEntry* free_entry = nullptr;
    for (EnvEntry& candidate : os_env_cache)
    {
        if (candidate.name[0] && cstr_equal(candidate.name, name))
        {
            entry = &candidate;
        }
        else if (!candidate.name[0] && !free_entry)
        {
            free_entry = &candidate;
        }
    }
    const char* result = nullptr;
    if (missing)
    {
        if (entry)
        {
            free(entry->value);
            *entry = {};
        }
    }
    else
    {
        entry = entry ? entry : free_entry;
        if (entry && (!entry->value || !cstr_equal(entry->value, value)))
        {
            free(entry->value);
            cstr_copy(entry->name, sizeof(entry->name), name);
            entry->value = _strdup(value);
        }
        result = entry ? entry->value : nullptr;
    }
    ReleaseSRWLockExclusive(&os_env_lock);
    return result;
}

// Both the process environment and the C runtime's copy, which _wputenv_s keeps in step
void os_set_env (const char* name, const char* value)
{
    wchar_t wide_name[256];
    wchar_t wide_value[OS_MAX_PATH];
    if (os_win_to_wide(name, wide_name, ARRAY_SIZE(wide_name)) && os_win_to_wide(value ? value : "", wide_value, ARRAY_SIZE(wide_value)))
    {
        _wputenv_s(wide_name, wide_value);
        SetEnvironmentVariableW(wide_name, wide_value);
    }
}

void os_unset_env (const char* name)
{
    wchar_t wide_name[256];
    if (os_win_to_wide(name, wide_name, ARRAY_SIZE(wide_name)))
    {
        _wputenv_s(wide_name, L"");
        SetEnvironmentVariableW(wide_name, nullptr);
    }
}

//
// Files and libraries
//

static DWORD file_attributes (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    return path && os_win_to_wide(path, wide, ARRAY_SIZE(wide)) ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
}

b8 os_file_exists (const char* path)
{
    const DWORD attributes = file_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

b8 os_directory_exists (const char* path)
{
    const DWORD attributes = file_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

OS_Library* os_library_open (const char* name)
{
    wchar_t wide[OS_MAX_PATH];
    return name && name[0] && os_win_to_wide(name, wide, ARRAY_SIZE(wide)) ? (OS_Library*)LoadLibraryW(wide) : nullptr;
}

b8 os_library_path (const char* name, char* out_path, usize size)
{
    (void)name;
    if (out_path && size > 0)
    {
        out_path[0] = 0;
    }
    return false;
}

// The app's loader rather than whichever the search path finds first
OS_Library* os_vulkan_loader ()
{
    return os_module_loaded("vulkan-1.dll") ? os_library_open("vulkan-1.dll") : nullptr;
}

void os_library_close (OS_Library* library)
{
    if (library)
    {
        FreeLibrary((HMODULE)library);
    }
}

b8 os_module_loaded (const char* name)
{
    wchar_t wide[OS_MAX_PATH];
    return name && name[0] && os_win_to_wide(name, wide, ARRAY_SIZE(wide)) && GetModuleHandleW(wide) != nullptr;
}

void* os_library_symbol (OS_Library* library, const char* name)
{
    if (!name || !name[0])
    {
        return nullptr;
    }
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
}

b8 os_module_directory (char* out_dir, usize out_size)
{
    char        path[OS_MAX_PATH] = {};
    wchar_t     wide[OS_MAX_PATH];
    const DWORD length = GetModuleFileNameW(this_module(), wide, ARRAY_SIZE(wide));
    if (length == 0 || length >= ARRAY_SIZE(wide) || !os_win_from_wide(wide, path, sizeof(path)))
    {
        return false;
    }
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

b8 os_list_directory (const char* path, void (*callback)(const char* name, void* user), void* user)
{
    ASSERT(callback);
    char    pattern[OS_MAX_PATH];
    wchar_t wide[OS_MAX_PATH];
    if (!os_directory_exists(path) || cstr_format(pattern, sizeof(pattern), "%s\\*", path) >= sizeof(pattern) || !os_win_to_wide(pattern, wide, ARRAY_SIZE(wide)))
    {
        return false;
    }
    WIN32_FIND_DATAW found  = {};
    const HANDLE     search = FindFirstFileW(wide, &found);
    if (search == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    do
    {
        char name[OS_MAX_PATH];
        if (os_win_from_wide(found.cFileName, name, sizeof(name)) && !cstr_equal(name, ".") && !cstr_equal(name, ".."))
        {
            callback(name, user);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return true;
}

namespace
{

struct DeleteContext
{
    const char* directory;
    b8          ok;
};

} // namespace

static void delete_entry (const char* name, void* user)
{
    DeleteContext& context = *(DeleteContext*)user;
    char           path[OS_MAX_PATH];
    context.ok = context.ok && cstr_format(path, sizeof(path), "%s\\%s", context.directory, name) < sizeof(path) && os_delete_tree(path);
}

// Links and junctions are removed, never followed
b8 os_delete_tree (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    if (!path || !path[0] || !os_win_to_wide(path, wide, ARRAY_SIZE(wide)))
    {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        DeleteContext context = { .directory = path, .ok = true };
        if (!os_list_directory(path, delete_entry, &context) || !context.ok)
        {
            return false;
        }
        return RemoveDirectoryW(wide) != 0;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        return RemoveDirectoryW(wide) != 0;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
    {
        SetFileAttributesW(wide, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    }
    return DeleteFileW(wide) != 0;
}

b8 os_read_file (const char* path, u8** out_data, usize* out_size)
{
    ASSERT(out_data && out_size);
    *out_data = nullptr;
    *out_size = 0;
    // Shares writing and deletion, so a file another handle writes (a recording, a lock file)
    // can be read as on POSIX
    wchar_t wide[OS_MAX_PATH];
    if (!path || !os_win_to_wide(path, wide, ARRAY_SIZE(wide)))
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
        data             = (u8*)os_memory_alloc(size + 1); // terminated, for text
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
}

b8 os_write_file (const char* path, const void* data, usize size)
{
    OS_File* file = os_file_create(path);
    if (!file)
    {
        return false;
    }
    const b8 written = os_file_write(file, data, size);
    return os_file_close(file) && written;
}

b8 os_write_file_atomic (const char* path, const void* data, usize size)
{
    char    temporary[OS_MAX_PATH];
    wchar_t wide_temporary[OS_MAX_PATH];
    wchar_t wide_path[OS_MAX_PATH];
    if (!path || cstr_format(temporary, sizeof(temporary), "%s.tmp", path) >= sizeof(temporary) || !os_win_to_wide(temporary, wide_temporary, ARRAY_SIZE(wide_temporary)) || !os_win_to_wide(path, wide_path, ARRAY_SIZE(wide_path)))
    {
        return false;
    }
    OS_File* file = os_file_create(temporary);
    if (!file)
    {
        return false;
    }
    const b8 written = os_file_write(file, data, size) && os_file_flush(file);
    if (!os_file_close(file) || !written || !MoveFileExW(wide_temporary, wide_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        os_delete_file(temporary);
        return false;
    }
    return true;
}

b8 os_current_directory (char* out_path, usize size)
{
    wchar_t     wide[OS_MAX_PATH];
    const DWORD length = GetCurrentDirectoryW(ARRAY_SIZE(wide), wide);
    char        current[OS_MAX_PATH * 3];
    if (length == 0 || length >= ARRAY_SIZE(wide) || !os_win_from_wide(wide, current, sizeof(current)))
    {
        return false;
    }
    const usize end = cstr_length(current);
    return cstr_format(out_path, size, end > 0 && (current[end - 1] == '\\' || current[end - 1] == '/') ? "%s" : "%s\\", current) < size;
}

// A drive letter with a separator, or a separator first (UNC and rooted paths)
b8 os_path_is_absolute (const char* path)
{
    if (!path)
    {
        return false;
    }
    const b8 drive = ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    return drive || path[0] == '\\' || path[0] == '/';
}

b8 os_create_temp_directory (const char* prefix, char* out_path, usize size)
{
    char base[OS_MAX_PATH];
    if (!os_temp_dir(base, sizeof(base)))
    {
        return false;
    }
    static OS_Atomic32 counter = {};
    for (u32 attempt = 0; attempt < 100; attempt++)
    {
        wchar_t   wide[OS_MAX_PATH];
        const u32 unique = (u32)os_atomic_add(&counter, 1);
        if (cstr_format(out_path, size, "%s\\%s_%u_%u_%llu", base, prefix ? prefix : "puppetxr", os_process_id(), unique, (unsigned long long)os_monotonic_ns()) >= size || !os_win_to_wide(out_path, wide, ARRAY_SIZE(wide)))
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
}

//
// Processes
//

// One argument as CommandLineToArgvW and the C runtime split it back: quoted when it has blanks,
// backslashes doubled before a quote
static void put_wide (wchar_t* line, usize capacity, usize& used, wchar_t c)
{
    if (used + 1 < capacity)
    {
        line[used++] = c;
    }
}

static b8 append_argument (wchar_t* line, usize capacity, usize& used, const char* argument)
{
    wchar_t wide[OS_MAX_PATH];
    if (!os_win_to_wide(argument, wide, ARRAY_SIZE(wide)))
    {
        return false;
    }
    b8 quote = wide[0] == 0;
    for (const wchar_t* c = wide; *c; c++)
    {
        quote = quote || *c == L' ' || *c == L'\t' || *c == L'"';
    }
    if (used > 0)
    {
        put_wide(line, capacity, used, L' ');
    }
    if (quote)
    {
        put_wide(line, capacity, used, L'"');
    }
    for (const wchar_t* c = wide;; c++)
    {
        usize backslashes = 0;
        while (*c == L'\\')
        {
            backslashes++;
            c++;
        }
        if (*c == 0)
        {
            for (usize i = 0; i < (quote ? backslashes * 2 : backslashes); i++)
            {
                put_wide(line, capacity, used, L'\\');
            }
            break;
        }
        for (usize i = 0; i < (*c == L'"' ? backslashes * 2 + 1 : backslashes); i++)
        {
            put_wide(line, capacity, used, L'\\');
        }
        put_wide(line, capacity, used, *c);
    }
    if (quote)
    {
        put_wide(line, capacity, used, L'"');
    }
    line[used] = 0;
    return used + 1 < capacity;
}

static HANDLE open_inherited (const char* path, b8 write)
{
    wchar_t wide[OS_MAX_PATH];
    if (!os_win_to_wide(path, wide, ARRAY_SIZE(wide)))
    {
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES inherit = { .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE };
    return CreateFileW(wide, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

OS_Process* os_process_spawn (const char* const* argv, const char* stdin_path, const char* stdout_path)
{
    if (!argv || !argv[0])
    {
        return nullptr;
    }
    static wchar_t line[32768];
    usize          used = 0;
    line[0]             = 0;
    for (const char* const* argument = argv; *argument; argument++)
    {
        if (!append_argument(line, ARRAY_SIZE(line), used, *argument))
        {
            LOG_ERROR("The command line of %s is too long", argv[0]);
            return nullptr;
        }
    }
    const HANDLE input  = stdin_path ? open_inherited(stdin_path, false) : GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = stdout_path ? open_inherited(stdout_path, true) : GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE errors = stdout_path ? output : GetStdHandle(STD_ERROR_HANDLE);
    OS_Process*  result = nullptr;
    if ((stdin_path && input == INVALID_HANDLE_VALUE) || (stdout_path && output == INVALID_HANDLE_VALUE))
    {
        LOG_ERROR("Cannot open the streams of %s: error %lu", argv[0], GetLastError());
    }
    else
    {
        STARTUPINFOW        startup     = { .cb = sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION information = {};
        startup.dwFlags                 = (stdin_path || stdout_path) ? STARTF_USESTDHANDLES : 0;
        startup.hStdInput               = input;
        startup.hStdOutput              = output;
        startup.hStdError               = errors;
        if (CreateProcessW(nullptr, line, nullptr, nullptr, (stdin_path || stdout_path) ? TRUE : FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &information))
        {
            CloseHandle(information.hThread);
            result          = (OS_Process*)os_memory_alloc(sizeof(OS_Process));
            result->process = information.hProcess;
        }
        else
        {
            LOG_ERROR("Cannot start %s: error %lu", argv[0], GetLastError());
        }
    }
    // The child has its own copies of the streams by now
    if (stdin_path && input != INVALID_HANDLE_VALUE)
    {
        CloseHandle(input);
    }
    if (stdout_path && output != INVALID_HANDLE_VALUE)
    {
        CloseHandle(output);
    }
    return result;
}

b8 os_process_wait (OS_Process* process, u32 timeout_ms, i32& out_exit_code)
{
    out_exit_code = -1;
    if (!process || WaitForSingleObject(process->process, timeout_ms) != WAIT_OBJECT_0)
    {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(process->process, &code);
    out_exit_code = (i32)code;
    return true;
}

void os_process_release (OS_Process* process)
{
    if (!process)
    {
        return;
    }
    if (WaitForSingleObject(process->process, 0) != WAIT_OBJECT_0)
    {
        TerminateProcess(process->process, 1);
        WaitForSingleObject(process->process, INFINITE);
    }
    CloseHandle(process->process);
    os_memory_release(process);
}

//
// Mutexes, semaphores and threads
//

OS_Mutex* os_mutex_create ()
{
    OS_Mutex* mutex = (OS_Mutex*)os_memory_alloc(sizeof(OS_Mutex));
    InitializeCriticalSection(&mutex->section);
    return mutex;
}

void os_mutex_destroy (OS_Mutex* mutex)
{
    if (mutex)
    {
        DeleteCriticalSection(&mutex->section);
        os_memory_release(mutex);
    }
}

b8 os_sys_mutex_try_lock (OS_Mutex* mutex)
{
    return TryEnterCriticalSection(&mutex->section) != 0;
}

void os_sys_mutex_lock (OS_Mutex* mutex)
{
    EnterCriticalSection(&mutex->section);
}

void os_sys_mutex_unlock (OS_Mutex* mutex)
{
    LeaveCriticalSection(&mutex->section);
}

OS_Semaphore* os_semaphore_create (u32 initial)
{
    return (OS_Semaphore*)CreateSemaphoreW(nullptr, (LONG)initial, LONG_MAX, nullptr);
}

void os_semaphore_destroy (OS_Semaphore* semaphore)
{
    if (semaphore)
    {
        CloseHandle((HANDLE)semaphore);
    }
}

void os_semaphore_wait (OS_Semaphore* semaphore)
{
    WaitForSingleObject((HANDLE)semaphore, INFINITE);
}

b8 os_sys_semaphore_wait_ns (OS_Semaphore* semaphore, u64 timeout_ns)
{
    const u64 ms = (timeout_ns + 999999ull) / 1000000ull;
    return WaitForSingleObject((HANDLE)semaphore, ms < INFINITE ? (DWORD)ms : INFINITE - 1) == WAIT_OBJECT_0;
}

b8 os_semaphore_try_wait (OS_Semaphore* semaphore)
{
    return WaitForSingleObject((HANDLE)semaphore, 0) == WAIT_OBJECT_0;
}

void os_semaphore_signal (OS_Semaphore* semaphore)
{
    ReleaseSemaphore((HANDLE)semaphore, 1, nullptr);
}

OS_Thread* os_thread_create (const char* name, OS_ThreadFunction* function, void* user)
{
    OS_Thread* thread = (OS_Thread*)os_memory_alloc(sizeof(OS_Thread));
    thread->function  = function;
    thread->user      = user;
    if (name)
    {
        os_win_to_wide(name, thread->name, ARRAY_SIZE(thread->name));
    }
    thread->handle = CreateThread(nullptr, 0, thread_trampoline, thread, 0, nullptr);
    if (!thread->handle)
    {
        os_memory_release(thread);
        return nullptr;
    }
    return thread;
}

i32 os_thread_join (OS_Thread* thread)
{
    if (!thread)
    {
        return 0;
    }
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    const i32 result = thread->result;
    os_memory_release(thread);
    return result;
}

u64 os_thread_current_id ()
{
    return (u64)GetCurrentThreadId();
}

void os_sys_thread_name (char* out_name, usize size)
{
    out_name[0] = 0;
    typedef HRESULT (WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);
    const GetThreadDescriptionFn get = (GetThreadDescriptionFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    PWSTR description = nullptr;
    if (get && SUCCEEDED(get(GetCurrentThread(), &description)) && description)
    {
        os_win_from_wide(description, out_name, size);
        LocalFree(description);
    }
}

//
// Logging, debugging and output capture
//

void os_sys_log_write (OS_LogLevel level, const char* text)
{
    static const char* const prefixes[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    char                     line[OS_LOG_LINE_LENGTH + 16];
    cstr_format(line, sizeof(line), "%s: %s\n", prefixes[(u32)level], text);
    OutputDebugStringA(line);
    fputs(line, stderr);
}

// Breaking without a debugger kills the host app, so only break when one is attached
void os_trigger_breakpoint ()
{
    if (IsDebuggerPresent())
    {
        __debugbreak();
    }
}

b8 os_default_audio_device (b8 input, wchar_t* out_id, usize count)
{
    if (!out_id || count == 0)
    {
        return false;
    }
    out_id[0] = 0;
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
}

// File descriptors of the C runtime, for capturing stdout and stderr; the UCRT is shared with
// the app (CMakeLists.txt), so its descriptors 1 and 2 are the app's
b8 os_sys_pipe (i32 out_fds[2])
{
    return _pipe(out_fds, 65536, _O_BINARY | _O_NOINHERIT) == 0;
}

i32 os_sys_fd_dup (i32 fd)
{
    return _dup(fd);
}

void os_sys_fd_dup2 (i32 from, i32 to)
{
    (void)_dup2(from, to);
}

void os_sys_fd_close (i32 fd)
{
    (void)_close(fd);
}

void os_sys_fd_write (i32 fd, const void* data, usize size)
{
    if (fd >= 0)
    {
        (void)_write(fd, data, (unsigned)size);
    }
}

// Pipes cannot be made non-blocking for _read, so only what is buffered is read
i64 os_sys_fd_read_available (i32 fd, void* data, usize capacity)
{
    DWORD available = 0;
    if (!PeekNamedPipe((HANDLE)_get_osfhandle(fd), nullptr, 0, nullptr, &available, nullptr) || available == 0)
    {
        return 0;
    }
    return _read(fd, data, (unsigned)(available < capacity ? available : capacity));
}

//
// Window thread wakeup
//

void os_sys_wake_open ()
{
    os_wake_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

void os_sys_wake_close ()
{
    if (os_wake_event)
    {
        CloseHandle(os_wake_event);
        os_wake_event = nullptr;
    }
}

void os_sys_wake_signal ()
{
    if (os_wake_event)
    {
        SetEvent(os_wake_event);
    }
}

void os_sys_wake_clear ()
{
    if (os_wake_event)
    {
        ResetEvent(os_wake_event);
    }
}

void os_sys_wake_wait (u64 timeout_ns)
{
    WaitForSingleObject(os_wake_event, (DWORD)(timeout_ns / 1000000ull));
    os_sys_wake_clear();
}

intptr_t os_sys_wake_handle ()
{
    return (intptr_t)os_wake_event;
}

//
// Streaming files
//

OS_File* os_file_create (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    if (!path || !path[0] || !os_win_to_wide(path, wide, ARRAY_SIZE(wide)))
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
// write one
OS_File* os_file_append (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    if (!path || !path[0] || !os_win_to_wide(path, wide, ARRAY_SIZE(wide)))
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

//
// Directories, sockets and lock files
//

b8 os_delete_file (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    return os_win_to_wide(path, wide, ARRAY_SIZE(wide)) && DeleteFileW(wide);
}

b8 os_create_directory (const char* path)
{
    wchar_t wide[OS_MAX_PATH];
    return os_win_to_wide(path, wide, ARRAY_SIZE(wide)) && (CreateDirectoryW(wide, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
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
    return os_win_from_wide(wide, out_path, size);
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
    if (!path || cstr_format(temporary, sizeof(temporary), "%s.%u.tmp", path, os_process_id()) >= sizeof(temporary) || !os_win_to_wide(temporary, wide_temporary, ARRAY_SIZE(wide_temporary)) || !os_win_to_wide(path, wide_path, ARRAY_SIZE(wide_path)))
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
    if (!os_win_to_wide(path, wide, ARRAY_SIZE(wide)))
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

#if !PUPPETXR_HEADLESS_ONLY

//
// Video: windows on the window thread, with their messages turned into raw events
//

#define WIN_CLASS_NAME  L"puppetxr"
// CS_BYTEALIGNCLIENT | CS_OWNDC: WGL needs a device context of the window's own
#define WIN_CLASS_STYLE (CS_BYTEALIGNCLIENT | CS_OWNDC)
#define WIN_TEXT_LIMIT  65536

namespace
{

struct VkWin32SurfaceCreateInfo
{
    i32         sType; // VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR
    const void* pNext;
    u32         flags;
    HINSTANCE   hinstance;
    HWND        hwnd;
};

typedef void* (*VkGetInstanceProcAddrFn)(void* instance, const char* name);
typedef i32   (*VkCreateSurfaceFn)(void* instance, const void* info, const void* allocator, u64* surface);

struct Win32Window
{
    HWND    hwnd;
    HDC     dc;
    b8      maximize_when_shown;
    b8      tracking;      // TrackMouseEvent asked for the leave
    b8      last_valid;
    f32     last_x;        // points
    f32     last_y;
    POINT   saved_cursor;  // where the cursor was when the relative mode began
    wchar_t high_surrogate;
    u32     buttons_down;
};

// IMM32 is loaded at runtime and pinned: user32 calls it for every new window without holding it,
// so an import of ours unloaded it with us in an app that does not import it, and a window made
// after on any thread (OBS's capture hook) crashed with STATUS_FATAL_USER_CALLBACK_EXCEPTION
struct ImmFunctions
{
    HIMC (WINAPI* ImmGetContext)            (HWND);
    BOOL (WINAPI* ImmReleaseContext)        (HWND, HIMC);
    BOOL (WINAPI* ImmSetCompositionWindow)  (HIMC, LPCOMPOSITIONFORM);
    BOOL (WINAPI* ImmSetCandidateWindow)    (HIMC, LPCANDIDATEFORM);
    BOOL (WINAPI* ImmAssociateContextEx)    (HWND, HIMC, DWORD);
};

struct Win32Video
{
    HMODULE      module;
    ATOM         window_class;
    b8           imm_loaded;
    ImmFunctions imm;
    b8           raw_registered;
    HCURSOR      cursors[(u32)OS_Cursor::Count];
    UINT (WINAPI* GetDpiForWindow)(HWND);
};

} // namespace

static Win32Video os_win = {};

static const char* const win_vulkan_extensions_list[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };

// Set 1 scan codes to keys; extended ones (E0 prefix) in the second table
static OS_Key win_key_from_scancode (u32 scancode, b8 extended, WPARAM vk)
{
    static const OS_Key plain[0x80] = {
        OS_Key::None, OS_Key::Escape, OS_Key::Digit1, OS_Key::Digit2, OS_Key::Digit3, OS_Key::Digit4, OS_Key::Digit5, OS_Key::Digit6,
        OS_Key::Digit7, OS_Key::Digit8, OS_Key::Digit9, OS_Key::Digit0, OS_Key::Minus, OS_Key::Equals, OS_Key::Backspace, OS_Key::Tab,
        OS_Key::Q, OS_Key::W, OS_Key::E, OS_Key::R, OS_Key::T, OS_Key::Y, OS_Key::U, OS_Key::I,
        OS_Key::O, OS_Key::P, OS_Key::LeftBracket, OS_Key::RightBracket, OS_Key::Enter, OS_Key::LeftCtrl, OS_Key::A, OS_Key::S,
        OS_Key::D, OS_Key::F, OS_Key::G, OS_Key::H, OS_Key::J, OS_Key::K, OS_Key::L, OS_Key::Semicolon,
        OS_Key::Apostrophe, OS_Key::Grave, OS_Key::LeftShift, OS_Key::Backslash, OS_Key::Z, OS_Key::X, OS_Key::C, OS_Key::V,
        OS_Key::B, OS_Key::N, OS_Key::M, OS_Key::Comma, OS_Key::Period, OS_Key::Slash, OS_Key::RightShift, OS_Key::KeypadMultiply,
        OS_Key::LeftAlt, OS_Key::Space, OS_Key::CapsLock, OS_Key::F1, OS_Key::F2, OS_Key::F3, OS_Key::F4, OS_Key::F5,
        OS_Key::F6, OS_Key::F7, OS_Key::F8, OS_Key::F9, OS_Key::F10, OS_Key::Pause, OS_Key::ScrollLock, OS_Key::Keypad7,
        OS_Key::Keypad8, OS_Key::Keypad9, OS_Key::KeypadMinus, OS_Key::Keypad4, OS_Key::Keypad5, OS_Key::Keypad6, OS_Key::KeypadPlus, OS_Key::Keypad1,
        OS_Key::Keypad2, OS_Key::Keypad3, OS_Key::Keypad0, OS_Key::KeypadDecimal, OS_Key::None, OS_Key::None, OS_Key::NonUsBackslash, OS_Key::F11,
        OS_Key::F12, OS_Key::KeypadEquals, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None,
        OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::F13, OS_Key::F14, OS_Key::F15, OS_Key::F16,
        OS_Key::F17, OS_Key::F18, OS_Key::F19, OS_Key::F20, OS_Key::F21, OS_Key::F22, OS_Key::F23, OS_Key::None,
        OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None,
        OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None,
    };
    // Num Lock and Pause share a scan code; the virtual key tells them apart
    if (vk == VK_NUMLOCK)
    {
        return OS_Key::NumLock;
    }
    if (vk == VK_PAUSE)
    {
        return OS_Key::Pause;
    }
    if (scancode == 0x76)
    {
        return OS_Key::F24;
    }
    if (!extended)
    {
        return scancode < ARRAY_SIZE(plain) ? plain[scancode] : OS_Key::None;
    }
    switch (scancode)
    {
    case 0x1C:
        return OS_Key::KeypadEnter;
    case 0x1D:
        return OS_Key::RightCtrl;
    case 0x35:
        return OS_Key::KeypadDivide;
    case 0x37:
        return OS_Key::PrintScreen;
    case 0x38:
        return OS_Key::RightAlt;
    case 0x47:
        return OS_Key::Home;
    case 0x48:
        return OS_Key::ArrowUp;
    case 0x49:
        return OS_Key::PageUp;
    case 0x4B:
        return OS_Key::ArrowLeft;
    case 0x4D:
        return OS_Key::ArrowRight;
    case 0x4F:
        return OS_Key::End;
    case 0x50:
        return OS_Key::ArrowDown;
    case 0x51:
        return OS_Key::PageDown;
    case 0x52:
        return OS_Key::Insert;
    case 0x53:
        return OS_Key::Delete;
    case 0x5B:
        return OS_Key::LeftSuper;
    case 0x5C:
        return OS_Key::RightSuper;
    case 0x5D:
        return OS_Key::Menu;
    default:
        return OS_Key::None;
    }
}

static b8 win_scancode_of_key (OS_Key key, u32& out_scancode, b8& out_extended)
{
    for (u32 extended = 0; extended < 2; extended++)
    {
        for (u32 scancode = 1; scancode < 0x80; scancode++)
        {
            if (win_key_from_scancode(scancode, extended != 0, 0) == key)
            {
                out_scancode = scancode;
                out_extended = extended != 0;
                return true;
            }
        }
    }
    return false;
}

static Win32Window* win_data (OS_Window* window)
{
    return (Win32Window*)window->data;
}

static u8 win_modifiers ()
{
    return (u8)(((GetKeyState(VK_SHIFT) & 0x8000) ? OS_MODIFIER_SHIFT : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? OS_MODIFIER_CTRL : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? OS_MODIFIER_ALT : 0) |
                (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? OS_MODIFIER_SUPER : 0));
}

// Pixels per point: the monitor's scale where the app made its threads DPI aware, else 1, as
// Windows then scales the window itself
static f32 win_density (HWND hwnd)
{
    typedef DPI_AWARENESS_CONTEXT (WINAPI* GetThreadDpiAwarenessContextFn)();
    typedef DPI_AWARENESS (WINAPI* GetAwarenessFn)(DPI_AWARENESS_CONTEXT);
    HMODULE                              user32    = GetModuleHandleW(L"user32.dll");
    const GetThreadDpiAwarenessContextFn context   = (GetThreadDpiAwarenessContextFn)(void*)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    const GetAwarenessFn                 awareness = (GetAwarenessFn)(void*)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    if (!context || !awareness || !os_win.GetDpiForWindow || awareness(context()) == DPI_AWARENESS_UNAWARE)
    {
        return 1.0f;
    }
    const UINT dpi = os_win.GetDpiForWindow(hwnd);
    return dpi > 0 ? (f32)dpi / 96.0f : 1.0f;
}

static void win_queue_size (OS_Window* window)
{
    RECT client = {};
    GetClientRect(win_data(window)->hwnd, &client);
    os_queue_resize(window, (u32)(client.right - client.left), (u32)(client.bottom - client.top), win_density(win_data(window)->hwnd));
}

static void win_register_raw_mouse (HWND hwnd, b8 on)
{
    RAWINPUTDEVICE device = {};
    device.usUsagePage    = 0x01; // generic desktop
    device.usUsage        = 0x02; // mouse
    device.dwFlags        = on ? 0 : RIDEV_REMOVE;
    device.hwndTarget     = on ? hwnd : nullptr;
    os_win.raw_registered = RegisterRawInputDevices(&device, 1, sizeof(device)) && on;
}

static void win_mouse_button (OS_Window* window, OS_Key key, b8 down, LPARAM lparam)
{
    Win32Window* data    = win_data(window);
    const f32    density = window->density > 0.0f ? window->density : 1.0f;
    const u32    bit     = 1u << ((u32)key - (u32)OS_Key::MouseLeft);
    data->buttons_down   = down ? data->buttons_down | bit : data->buttons_down & ~bit;
    // Captured while a button is down, so the release comes even outside the window
    if (down)
    {
        SetCapture(data->hwnd);
    }
    else if (data->buttons_down == 0)
    {
        ReleaseCapture();
    }
    os_queue_event(window, { .type = down ? OS_RawEventType::MouseButtonDown : OS_RawEventType::MouseButtonUp, .key = key, .modifiers = win_modifiers(), .x = (f32)GET_X_LPARAM(lparam) / density, .y = (f32)GET_Y_LPARAM(lparam) / density });
}

static void win_key (OS_Window* window, WPARAM wparam, LPARAM lparam, b8 down)
{
    const u32    scancode = (u32)(lparam >> 16) & 0xFF;
    const b8     extended = ((lparam >> 24) & 1) != 0;
    const b8     repeat   = down && ((lparam >> 30) & 1) != 0;
    const OS_Key key      = win_key_from_scancode(scancode, extended, wparam);
    if (key != OS_Key::None)
    {
        os_queue_event(window, { .type = down ? OS_RawEventType::KeyDown : OS_RawEventType::KeyUp, .key = key, .modifiers = win_modifiers(), .repeat = repeat });
    }
}

static void win_char (OS_Window* window, WPARAM wparam)
{
    Win32Window*  data = win_data(window);
    const wchar_t unit = (wchar_t)wparam;
    if (unit >= 0xD800 && unit < 0xDC00)
    {
        data->high_surrogate = unit;
        return;
    }
    wchar_t text[3] = { unit, 0, 0 };
    if (unit >= 0xDC00 && unit < 0xE000)
    {
        if (!data->high_surrogate)
        {
            return;
        }
        text[0] = data->high_surrogate;
        text[1] = unit;
    }
    data->high_surrogate = 0;
    if (text[0] < 0x20 || text[0] == 0x7F)
    {
        return;
    }
    OS_RawEvent event = { .type = OS_RawEventType::Text };
    if (os_win_from_wide(text, event.text, sizeof(event.text)))
    {
        os_queue_event(window, event);
    }
}

static LRESULT CALLBACK win_procedure (HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create = (const CREATESTRUCTW*)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    }
    OS_Window* window = (OS_Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!window || !window->data)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    Win32Window* data    = win_data(window);
    const f32    density = window->density > 0.0f ? window->density : 1.0f;
    switch (message)
    {
    case WM_CLOSE:
        os_queue_event(window, { .type = OS_RawEventType::WindowClose });
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
        {
            win_queue_size(window);
        }
        return 0;
    case WM_DPICHANGED:
    {
        const RECT* suggested = (const RECT*)lparam;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        win_queue_size(window);
        return 0;
    }
    case WM_SETFOCUS:
        os_queue_event(window, { .type = OS_RawEventType::FocusGained });
        return 0;
    case WM_KILLFOCUS:
        os_queue_event(window, { .type = OS_RawEventType::FocusLost });
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        win_key(window, wparam, lparam, true);
        // Alt+F4 closes through the default procedure; other system keys must not open the menu
        if (message == WM_SYSKEYDOWN && wparam == VK_F4)
        {
            break;
        }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        win_key(window, wparam, lparam, false);
        return 0;
    case WM_CHAR:
        win_char(window, wparam);
        return 0;
    case WM_MOUSEMOVE:
    {
        if (!data->tracking)
        {
            TRACKMOUSEEVENT track = { .cbSize = sizeof(TRACKMOUSEEVENT), .dwFlags = TME_LEAVE, .hwndTrack = hwnd, .dwHoverTime = 0 };
            data->tracking        = TrackMouseEvent(&track) != 0;
        }
        const f32 x = (f32)GET_X_LPARAM(lparam) / density;
        const f32 y = (f32)GET_Y_LPARAM(lparam) / density;
        if (!window->relative)
        {
            os_queue_event(window, { .type = OS_RawEventType::MouseMotion, .modifiers = win_modifiers(), .x = x, .y = y, .dx = data->last_valid ? x - data->last_x : 0.0f, .dy = data->last_valid ? y - data->last_y : 0.0f });
        }
        data->last_x     = x;
        data->last_y     = y;
        data->last_valid = true;
        return 0;
    }
    case WM_MOUSELEAVE:
        data->tracking   = false;
        data->last_valid = false;
        if (!window->relative)
        {
            os_queue_event(window, { .type = OS_RawEventType::MouseLeave });
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        win_mouse_button(window, OS_Key::MouseLeft, message == WM_LBUTTONDOWN, lparam);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        win_mouse_button(window, OS_Key::MouseRight, message == WM_RBUTTONDOWN, lparam);
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        win_mouse_button(window, OS_Key::MouseMiddle, message == WM_MBUTTONDOWN, lparam);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        win_mouse_button(window, GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? OS_Key::MouseX1 : OS_Key::MouseX2, message == WM_XBUTTONDOWN, lparam);
        return TRUE;
    case WM_MOUSEWHEEL:
        os_queue_event(window, { .type = OS_RawEventType::MouseWheel, .modifiers = win_modifiers(), .dy = (f32)GET_WHEEL_DELTA_WPARAM(wparam) / (f32)WHEEL_DELTA });
        return 0;
    case WM_MOUSEHWHEEL:
        os_queue_event(window, { .type = OS_RawEventType::MouseWheel, .modifiers = win_modifiers(), .dx = (f32)GET_WHEEL_DELTA_WPARAM(wparam) / (f32)WHEEL_DELTA });
        return 0;
    case WM_INPUT:
    {
        RAWINPUT input = {};
        UINT     size  = sizeof(input);
        if (window->relative && GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 && input.header.dwType == RIM_TYPEMOUSE &&
            !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && (input.data.mouse.lLastX != 0 || input.data.mouse.lLastY != 0))
        {
            os_queue_event(window, { .type = OS_RawEventType::MouseMotion, .modifiers = win_modifiers(), .x = data->last_x, .y = data->last_y, .dx = (f32)input.data.mouse.lLastX, .dy = (f32)input.data.mouse.lLastY });
        }
        break;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT)
        {
            SetCursor(window->relative ? nullptr : os_win.cursors[(u32)window->cursor]);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static b8 win_init ()
{
    os_win.module = this_module();
    WNDCLASSEXW window_class   = { .cbSize = sizeof(WNDCLASSEXW) };
    window_class.style         = WIN_CLASS_STYLE;
    window_class.lpfnWndProc   = win_procedure;
    window_class.hInstance     = os_win.module;
    window_class.hIcon         = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    window_class.hCursor       = nullptr;
    window_class.lpszClassName = WIN_CLASS_NAME;
    os_win.window_class        = RegisterClassExW(&window_class);
    if (!os_win.window_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LOG_WARNING("Cannot register the window class: error %lu", GetLastError());
        return false;
    }
    static const LPCWSTR shapes[] = { (LPCWSTR)IDC_ARROW, (LPCWSTR)IDC_IBEAM, (LPCWSTR)IDC_SIZEALL, (LPCWSTR)IDC_SIZENS, (LPCWSTR)IDC_SIZEWE, (LPCWSTR)IDC_SIZENESW, (LPCWSTR)IDC_SIZENWSE, (LPCWSTR)IDC_HAND, (LPCWSTR)IDC_WAIT, (LPCWSTR)IDC_APPSTARTING, (LPCWSTR)IDC_NO };
    static_assert(ARRAY_SIZE(shapes) == (u32)OS_Cursor::Hidden);
    for (u32 i = 0; i < ARRAY_SIZE(shapes); i++)
    {
        os_win.cursors[i] = LoadCursorW(nullptr, shapes[i]);
    }
    os_win.cursors[(u32)OS_Cursor::Hidden] = nullptr;
    os_win.GetDpiForWindow                 = (UINT (WINAPI*)(HWND))(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (!os_win.imm_loaded)
    {
        HMODULE imm = nullptr;
        if (LoadLibraryW(L"imm32.dll") && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"imm32.dll", &imm) && imm)
        {
            os_win.imm.ImmGetContext           = (HIMC (WINAPI*)(HWND))(void*)GetProcAddress(imm, "ImmGetContext");
            os_win.imm.ImmReleaseContext       = (BOOL (WINAPI*)(HWND, HIMC))(void*)GetProcAddress(imm, "ImmReleaseContext");
            os_win.imm.ImmSetCompositionWindow = (BOOL (WINAPI*)(HIMC, LPCOMPOSITIONFORM))(void*)GetProcAddress(imm, "ImmSetCompositionWindow");
            os_win.imm.ImmSetCandidateWindow   = (BOOL (WINAPI*)(HIMC, LPCANDIDATEFORM))(void*)GetProcAddress(imm, "ImmSetCandidateWindow");
            os_win.imm.ImmAssociateContextEx   = (BOOL (WINAPI*)(HWND, HIMC, DWORD))(void*)GetProcAddress(imm, "ImmAssociateContextEx");
            os_win.imm_loaded = os_win.imm.ImmGetContext && os_win.imm.ImmReleaseContext && os_win.imm.ImmSetCompositionWindow && os_win.imm.ImmSetCandidateWindow && os_win.imm.ImmAssociateContextEx;
        }
    }
    return true;
}

static void win_quit ()
{
    if (os_win.raw_registered)
    {
        win_register_raw_mouse(nullptr, false);
    }
    // The class never outlives the library's window procedure
    UnregisterClassW(WIN_CLASS_NAME, os_win.module);
    os_win.window_class = 0;
}

static b8 win_create_window (OS_Window* window, const OS_WindowDesc& desc)
{
    Win32Window* data = (Win32Window*)os_memory_alloc(sizeof(Win32Window));
    window->data      = data;
    wchar_t title[256];
    if (!os_win_to_wide(desc.title, title, ARRAY_SIZE(title)))
    {
        title[0] = 0;
    }
    const DWORD style = WS_OVERLAPPEDWINDOW;
    data->hwnd        = CreateWindowExW(0, WIN_CLASS_NAME, title, style, CW_USEDEFAULT, CW_USEDEFAULT, (i32)desc.width, (i32)desc.height, nullptr, nullptr, os_win.module, window);
    if (!data->hwnd)
    {
        LOG_ERROR("CreateWindowExW failed: error %lu", GetLastError());
        os_memory_release(data);
        window->data = nullptr;
        return false;
    }
    // The client area gets the size, in points, at the window's scale
    window->density = win_density(data->hwnd);
    RECT frame      = { 0, 0, (LONG)((f32)desc.width * window->density + 0.5f), (LONG)((f32)desc.height * window->density + 0.5f) };
    AdjustWindowRectEx(&frame, style, FALSE, 0);
    SetWindowPos(data->hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    data->dc = GetDC(data->hwnd);
    win_queue_size(window);
    window->hidden = true;
    if (!desc.hidden)
    {
        window->driver->show(window);
        window->hidden = false;
    }
    return true;
}

static void win_destroy_window (OS_Window* window)
{
    Win32Window* data = win_data(window);
    if (window->relative)
    {
        ClipCursor(nullptr);
    }
    ReleaseDC(data->hwnd, data->dc);
    SetWindowLongPtrW(data->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(data->hwnd);
    os_memory_release(data);
    window->data = nullptr;
}

static b8 win_native_handles (OS_Window* window, OS_WindowNative& out_native)
{
    out_native.window  = (u64)(uintptr_t)win_data(window)->hwnd;
    out_native.display = win_data(window)->dc;
    return true;
}

static f32 win_refresh_rate (OS_Window* window)
{
    MONITORINFOEXW monitor = {};
    monitor.cbSize         = sizeof(monitor);
    DEVMODEW mode          = {};
    mode.dmSize            = sizeof(mode);
    if (!GetMonitorInfoW(MonitorFromWindow(win_data(window)->hwnd, MONITOR_DEFAULTTONEAREST), &monitor) || !EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode))
    {
        return 0.0f;
    }
    return mode.dmDisplayFrequency > 1 ? (f32)mode.dmDisplayFrequency : 0.0f;
}

static const char* const* win_vulkan_extensions (u32& out_count)
{
    out_count = ARRAY_SIZE(win_vulkan_extensions_list);
    return win_vulkan_extensions_list;
}

static b8 win_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface)
{
    OS_Library*                   loader = vk_get_instance_proc_addr ? nullptr : os_vulkan_loader();
    const VkGetInstanceProcAddrFn get    = vk_get_instance_proc_addr ? (VkGetInstanceProcAddrFn)(uintptr_t)vk_get_instance_proc_addr : (VkGetInstanceProcAddrFn)os_library_symbol(loader, "vkGetInstanceProcAddr");
    const VkCreateSurfaceFn       create = get ? (VkCreateSurfaceFn)get(vk_instance, "vkCreateWin32SurfaceKHR") : nullptr;
    const VkWin32SurfaceCreateInfo info  = { .sType = 1000009000, .pNext = nullptr, .flags = 0, .hinstance = os_win.module, .hwnd = win_data(window)->hwnd };
    const i32                     result = create ? create(vk_instance, &info, nullptr, &out_surface) : -1;
    os_library_close(loader);
    if (result != 0)
    {
        LOG_ERROR("vkCreateWin32SurfaceKHR failed or is missing: %d", result);
        return false;
    }
    return true;
}

// Raw input moves while held; the cursor hides, stays in the client area (os_window_keep_cursor)
// and comes back where looking began
static void win_set_relative_mouse (OS_Window* window, b8 relative)
{
    Win32Window* data = win_data(window);
    if (relative)
    {
        GetCursorPos(&data->saved_cursor);
        win_register_raw_mouse(data->hwnd, true);
        RECT  client = {};
        POINT corner = {};
        GetClientRect(data->hwnd, &client);
        ClientToScreen(data->hwnd, &corner);
        OffsetRect(&client, corner.x, corner.y);
        ClipCursor(&client);
        SetCursor(nullptr);
    }
    else
    {
        win_register_raw_mouse(data->hwnd, false);
        ClipCursor(nullptr);
        SetCursorPos(data->saved_cursor.x, data->saved_cursor.y);
        SetCursor(os_win.cursors[(u32)window->cursor]);
        data->last_valid = false;
    }
}

static b8 win_maximize (OS_Window* window)
{
    Win32Window* data = win_data(window);
    if (!IsWindowVisible(data->hwnd))
    {
        data->maximize_when_shown = true;
        return true;
    }
    ShowWindow(data->hwnd, SW_MAXIMIZE);
    return true;
}

static b8 win_maximized (OS_Window* window)
{
    return win_data(window)->maximize_when_shown || IsZoomed(win_data(window)->hwnd);
}

static void win_show (OS_Window* window)
{
    Win32Window* data = win_data(window);
    ShowWindow(data->hwnd, data->maximize_when_shown ? SW_SHOWMAXIMIZED : SW_SHOW);
    data->maximize_when_shown = false;
    SetForegroundWindow(data->hwnd);
    BringWindowToTop(data->hwnd);
    win_queue_size(window);
}

static void win_hide (OS_Window* window)
{
    ShowWindow(win_data(window)->hwnd, SW_HIDE);
}

static void win_set_cursor (OS_Window* window, OS_Cursor cursor)
{
    POINT point = {};
    if (!window->relative && GetCursorPos(&point) && WindowFromPoint(point) == win_data(window)->hwnd)
    {
        SetCursor(os_win.cursors[(u32)cursor]);
    }
}

// The input method follows text input: on while a text field is active, its candidates under the
// cursor, off otherwise
static void win_text_input (OS_Window* window, b8 active, f32 x, f32 y, f32 height)
{
    if (!os_win.imm_loaded)
    {
        return;
    }
    const HWND hwnd = win_data(window)->hwnd;
    os_win.imm.ImmAssociateContextEx(hwnd, nullptr, active ? IACE_DEFAULT : 0);
    if (!active)
    {
        return;
    }
    const HIMC context = os_win.imm.ImmGetContext(hwnd);
    if (!context)
    {
        return;
    }
    const f32       density     = window->density > 0.0f ? window->density : 1.0f;
    COMPOSITIONFORM composition = {};
    composition.dwStyle         = CFS_FORCE_POSITION;
    composition.ptCurrentPos    = { (LONG)(x * density), (LONG)(y * density) };
    os_win.imm.ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate = {};
    candidate.dwStyle       = CFS_EXCLUDE;
    candidate.ptCurrentPos  = { (LONG)(x * density), (LONG)(y * density) };
    candidate.rcArea        = { (LONG)(x * density), (LONG)(y * density), (LONG)(x * density) + 1, (LONG)((y + height) * density) };
    os_win.imm.ImmSetCandidateWindow(context, &candidate);
    os_win.imm.ImmReleaseContext(hwnd, context);
}

// Another process may hold the clipboard for a moment
static b8 win_open_clipboard (HWND hwnd)
{
    for (u32 attempt = 0; attempt < 10; attempt++)
    {
        if (OpenClipboard(hwnd))
        {
            return true;
        }
        Sleep(2);
    }
    return false;
}

static b8 win_clipboard_text (OS_Window* window, char* out_text, usize size)
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !win_open_clipboard(win_data(window)->hwnd))
    {
        return false;
    }
    b8           fits   = false;
    const HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    const wchar_t* text = memory ? (const wchar_t*)GlobalLock(memory) : nullptr;
    if (text)
    {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        fits             = needed > 0 && (usize)needed <= size && os_win_from_wide(text, out_text, size);
        GlobalUnlock(memory);
    }
    CloseClipboard();
    return fits;
}

static void win_set_clipboard_text (OS_Window* window, const char* text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0 || length > WIN_TEXT_LIMIT || !win_open_clipboard(win_data(window)->hwnd))
    {
        return;
    }
    EmptyClipboard();
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)length * sizeof(wchar_t));
    wchar_t*      wide   = memory ? (wchar_t*)GlobalLock(memory) : nullptr;
    if (wide)
    {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory))
        {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

static b8 win_cursor_position (OS_Window* window, f32& out_x, f32& out_y)
{
    const HWND hwnd   = win_data(window)->hwnd;
    POINT      cursor = {};
    if (!GetCursorPos(&cursor) || WindowFromPoint(cursor) != hwnd || !ScreenToClient(hwnd, &cursor))
    {
        return false;
    }
    const f32 density = window->density > 0.0f ? window->density : 1.0f;
    out_x             = (f32)cursor.x / density;
    out_y             = (f32)cursor.y / density;
    return true;
}

static void win_keep_cursor (OS_Window* window, b8 held)
{
    const HWND hwnd   = win_data(window)->hwnd;
    RECT       clip   = {};
    RECT       client = {};
    POINT      corner = {};
    if (!GetClipCursor(&clip) || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &corner))
    {
        return;
    }
    OffsetRect(&client, corner.x, corner.y);
    const i32           desktop_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const i32           desktop_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const OS_ScreenRect desktop   = { desktop_x, desktop_y, desktop_x + GetSystemMetrics(SM_CXVIRTUALSCREEN), desktop_y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    switch (os_cursor_clip_action(held, GetForegroundWindow() == hwnd, { clip.left, clip.top, clip.right, clip.bottom }, { client.left, client.top, client.right, client.bottom }, desktop))
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
}

// What the key prints on the layout in use; dead keys give their accent
static b8 win_key_label (OS_Key key, char* out, usize size)
{
    u32 scancode = 0;
    b8  extended = false;
    if (!win_scancode_of_key(key, scancode, extended) || !((key >= OS_Key::A && key <= OS_Key::Digit9) || key == OS_Key::Grave || key == OS_Key::LeftBracket || key == OS_Key::RightBracket || (key >= OS_Key::Minus && key <= OS_Key::NonUsBackslash)))
    {
        return false;
    }
    const UINT vk        = MapVirtualKeyW(scancode | (extended ? 0xE000u : 0u), MAPVK_VSC_TO_VK_EX);
    const UINT character = vk ? MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu : 0;
    if (character <= 0x20)
    {
        return false;
    }
    wchar_t text[2] = { (wchar_t)character, 0 };
    CharUpperW(text);
    return os_win_from_wide(text, out, size);
}

static void win_pump ()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// A message to the thread ends the wait as the wake does
static void win_wait (u64 timeout_ns)
{
    const HANDLE wake = (HANDLE)os_sys_wake_handle();
    MsgWaitForMultipleObjectsEx(1, &wake, (DWORD)(timeout_ns / 1000000ull), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    os_sys_wake_clear();
}

const OS_VideoDriver os_windows_driver = {
    .name                  = "windows",
    .init                  = win_init,
    .quit                  = win_quit,
    .create_window         = win_create_window,
    .destroy_window        = win_destroy_window,
    .native_handles        = win_native_handles,
    .refresh_rate          = win_refresh_rate,
    .vulkan_extensions     = win_vulkan_extensions,
    .create_vulkan_surface = win_create_vulkan_surface,
    .set_relative_mouse    = win_set_relative_mouse,
    .maximize              = win_maximize,
    .maximized             = win_maximized,
    .show                  = win_show,
    .hide                  = win_hide,
    .set_cursor            = win_set_cursor,
    .text_input            = win_text_input,
    .clipboard_text        = win_clipboard_text,
    .set_clipboard_text    = win_set_clipboard_text,
    .cursor_position       = win_cursor_position,
    .keep_cursor           = win_keep_cursor,
    .key_label             = win_key_label,
    .pump                  = win_pump,
    .wait                  = win_wait,
};

#else

static b8 no_display ()
{
    return false;
}

const OS_VideoDriver os_windows_driver = { .name = "windows", .init = no_display };

#endif
