#include "platform_internal.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if HAVE_MACOS
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

#if HAVE_MACOS
extern char** environ;
#endif

//
// Types
//

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

struct OS_File
{
    FILE* file;
};

struct OS_Mutex
{
    pthread_mutex_t mutex;
};

// macOS has no unnamed POSIX semaphores, so every Unix counts under a mutex
struct OS_Semaphore
{
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    u32             count;
};

struct OS_Thread
{
    pthread_t          thread;
    OS_ThreadFunction* function;
    void*              user;
    i32                result;
    char               name[16]; // what Linux keeps of a thread name
};

struct OS_Process
{
    pid_t pid;
    b8    exited;
    i32   exit_code;
};

namespace
{

// strerror is not thread safe; glibc's strerror_r returns the text, POSIX's fills the buffer
struct ErrnoText
{
    char text[128] = {};
};

} // namespace

//
// Statics
//

#if HAVE_LINUX
static i32 os_wake_fds[2] = { -1, -1 };
#endif

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

// Absolute time for pthread_cond_timedwait, whose clock is the realtime one on macOS
static timespec deadline_after (u64 ns)
{
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    const u64 total = (u64)now.tv_nsec + ns % 1000000000ull;
    timespec  out   = {};
    out.tv_sec      = now.tv_sec + (time_t)(ns / 1000000000ull) + (time_t)(total / 1000000000ull);
    out.tv_nsec     = (long)(total % 1000000000ull);
    return out;
}

static void* thread_trampoline (void* user)
{
    OS_Thread* thread = (OS_Thread*)user;
#if HAVE_MACOS
    pthread_setname_np(thread->name);
#else
    pthread_setname_np(pthread_self(), thread->name);
#endif
    thread->result = thread->function(thread->user);
    return nullptr;
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

u64 os_monotonic_ns ()
{
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000000000ull + (u64)now.tv_nsec;
}

u64 os_timespec_ns ()
{
    return os_monotonic_ns();
}

OS_NativeClock os_native_clock ()
{
    return OS_NativeClock::Timespec;
}

u64 os_native_ticks_to_ns (i64 ticks)
{
    return ticks <= 0 ? 0 : (u64)ticks;
}

i64 os_ns_to_native_ticks (u64 ns)
{
    return (i64)ns;
}

void os_sleep_ns (u64 ns)
{
    timespec rest = { .tv_sec = (time_t)(ns / 1000000000ull), .tv_nsec = (long)(ns % 1000000000ull) };
    while (nanosleep(&rest, &rest) != 0 && errno == EINTR)
    {
    }
}

const char* os_env (const char* name)
{
    return name ? getenv(name) : nullptr; // NOLINT(concurrency-mt-unsafe): platform.h says the environment is set before threads start
}

void os_set_env (const char* name, const char* value)
{
    if (name)
    {
        setenv(name, value ? value : "", 1); // NOLINT(concurrency-mt-unsafe): platform.h says to set it before threads start
    }
}

void os_unset_env (const char* name)
{
    if (name)
    {
        unsetenv(name); // NOLINT(concurrency-mt-unsafe): platform.h says to set it before threads start
    }
}

//
// Files and libraries
//

b8 os_file_exists (const char* path)
{
    struct stat info = {};
    return path && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

b8 os_directory_exists (const char* path)
{
    struct stat info = {};
    return path && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
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
#if HAVE_MACOS
    const char* loaded = loaded_image_path(name);
    if (loaded)
    {
        name = loaded;
    }
#endif
    return (OS_Library*)dlopen(name, RTLD_NOW | RTLD_LOCAL);
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

#if HAVE_MACOS
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
    if (library)
    {
        dlclose(library);
    }
}

b8 os_module_loaded (const char* name)
{
    if (!name || !name[0])
    {
        return false;
    }
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
}

void* os_library_symbol (OS_Library* library, const char* name)
{
    if (!name || !name[0])
    {
        return nullptr;
    }
    return dlsym(library ? (void*)library : RTLD_DEFAULT, name);
}

b8 os_module_directory (char* out_dir, usize out_size)
{
    char    path[OS_MAX_PATH] = {};
    Dl_info info              = {};
    if (!dladdr((const void*)&os_module_directory, &info) || !info.dli_fname || !cstr_copy(path, sizeof(path), info.dli_fname))
    {
        return false;
    }
    usize end = 0;
    for (usize i = 0; path[i]; i++)
    {
        if (path[i] == '/')
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
    DIR* directory = path ? opendir(path) : nullptr;
    if (!directory)
    {
        return false;
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): readdir is safe on a stream of this call alone
    for (const dirent* entry = readdir(directory); entry; entry = readdir(directory))
    {
        if (!cstr_equal(entry->d_name, ".") && !cstr_equal(entry->d_name, ".."))
        {
            callback(entry->d_name, user);
        }
    }
    closedir(directory);
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
    context.ok = context.ok && cstr_format(path, sizeof(path), "%s/%s", context.directory, name) < sizeof(path) && os_delete_tree(path);
}

// Links are removed, never followed
b8 os_delete_tree (const char* path)
{
    if (!path || !path[0])
    {
        return false;
    }
    struct stat info = {};
    if (lstat(path, &info) != 0)
    {
        return errno == ENOENT;
    }
    if (S_ISDIR(info.st_mode))
    {
        DeleteContext context = { .directory = path, .ok = true };
        if (!os_list_directory(path, delete_entry, &context) || !context.ok)
        {
            return false;
        }
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

b8 os_read_file (const char* path, u8** out_data, usize* out_size)
{
    ASSERT(out_data && out_size);
    *out_data = nullptr;
    *out_size = 0;
    const i32 fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0)
    {
        return false;
    }
    // Grows as it reads: files under /proc report no size
    usize capacity = 4096;
    usize size     = 0;
    u8*   data     = (u8*)os_memory_alloc(capacity + 1);
    for (;;)
    {
        if (size == capacity)
        {
            capacity *= 2;
            data = (u8*)os_memory_realloc(data, capacity + 1);
        }
        const ssize_t count = read(fd, data + size, capacity - size);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0)
        {
            close(fd);
            os_memory_release(data);
            return false;
        }
        if (count == 0)
        {
            break;
        }
        size += (usize)count;
    }
    close(fd);
    data[size] = 0; // terminated, for text
    *out_data  = data;
    *out_size  = size;
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
    if (!os_file_close(file) || !written || rename(temporary, path) != 0)
    {
        os_delete_file(temporary);
        return false;
    }
    return true;
}

OS_File* os_file_create (const char* path)
{
    if (!path || !path[0])
    {
        return nullptr;
    }
    FILE* stream = fopen(path, "wb");
    if (!stream)
    {
        LOG_ERROR("Cannot create %s: %s", path, errno_text(errno).text);
        return nullptr;
    }
    fcntl(fileno(stream), F_SETFD, FD_CLOEXEC);
    OS_File* file = (OS_File*)os_memory_alloc(sizeof(OS_File));
    file->file    = stream;
    return file;
}

OS_File* os_file_append (const char* path)
{
    if (!path || !path[0])
    {
        return nullptr;
    }
    FILE* stream = fopen(path, "ab");
    if (!stream)
    {
        return nullptr;
    }
    fcntl(fileno(stream), F_SETFD, FD_CLOEXEC);
    OS_File* file = (OS_File*)os_memory_alloc(sizeof(OS_File));
    file->file    = stream;
    return file;
}

b8 os_file_write (OS_File* file, const void* data, usize size) // NOLINT(misc-const-correctness): the signature of every platform
{
    if (!file || (!data && size > 0))
    {
        return false;
    }
    return size == 0 || fwrite(data, 1, size, file->file) == size;
}

b8 os_file_flush (OS_File* file) // NOLINT(misc-const-correctness): the signature of every platform
{
    return file && fflush(file->file) == 0;
}

b8 os_file_close (OS_File* file)
{
    if (!file)
    {
        return false;
    }
    const b8 failed = ferror(file->file) != 0;
    const b8 closed = fclose(file->file) == 0;
    os_memory_release(file);
    return closed && !failed;
}

b8 os_current_directory (char* out_path, usize size)
{
    char current[OS_MAX_PATH];
    if (!getcwd(current, sizeof(current)))
    {
        return false;
    }
    const usize length = cstr_length(current);
    return cstr_format(out_path, size, length > 0 && current[length - 1] == '/' ? "%s" : "%s/", current) < size;
}

b8 os_path_is_absolute (const char* path)
{
    return path && path[0] == '/';
}

b8 os_create_temp_directory (const char* prefix, char* out_path, usize size)
{
    char base[OS_MAX_PATH];
    if (!os_temp_dir(base, sizeof(base)))
    {
        return false;
    }
    return cstr_format(out_path, size, "%s/%s_XXXXXX", base, prefix ? prefix : "puppetxr") < size && mkdtemp(out_path) != nullptr;
}

//
// Processes
//

// Without paths the child shares our streams; with stdout_path it gets stderr too
OS_Process* os_process_spawn (const char* const* argv, const char* stdin_path, const char* stdout_path)
{
    if (!argv || !argv[0])
    {
        return nullptr;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (stdin_path)
    {
        posix_spawn_file_actions_addopen(&actions, 0, stdin_path, O_RDONLY, 0);
    }
    if (stdout_path)
    {
        posix_spawn_file_actions_addopen(&actions, 1, stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }
    pid_t     pid    = 0;
    const int result = posix_spawnp(&pid, argv[0], &actions, nullptr, (char* const*)(uintptr_t)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0)
    {
        LOG_ERROR("Cannot start %s: %s", argv[0], errno_text(result).text);
        return nullptr;
    }
    OS_Process* process = (OS_Process*)os_memory_alloc(sizeof(OS_Process));
    process->pid        = pid;
    return process;
}

static b8 reap (OS_Process* process, b8 block)
{
    if (process->exited)
    {
        return true;
    }
    int   status = 0;
    pid_t done   = 0;
    do
    {
        done = waitpid(process->pid, &status, block ? 0 : WNOHANG);
    } while (done < 0 && errno == EINTR);
    if (done != process->pid)
    {
        return false;
    }
    process->exited    = true;
    process->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? -WTERMSIG(status) : -1);
    return true;
}

b8 os_process_wait (OS_Process* process, u32 timeout_ms, i32& out_exit_code)
{
    out_exit_code = -1;
    if (!process)
    {
        return false;
    }
    const u64 deadline = os_monotonic_ns() + (u64)timeout_ms * 1000000ull;
    while (!reap(process, false))
    {
        if (os_monotonic_ns() >= deadline)
        {
            return false;
        }
        os_sleep_ns(1000000);
    }
    out_exit_code = process->exit_code;
    return true;
}

void os_process_release (OS_Process* process)
{
    if (!process)
    {
        return;
    }
    if (!reap(process, false))
    {
        kill(process->pid, SIGKILL);
        reap(process, true);
    }
    os_memory_release(process);
}

//
// Mutexes, semaphores and threads
//

OS_Mutex* os_mutex_create ()
{
    OS_Mutex* mutex = (OS_Mutex*)os_memory_alloc(sizeof(OS_Mutex));
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex->mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return mutex;
}

void os_mutex_destroy (OS_Mutex* mutex)
{
    if (mutex)
    {
        pthread_mutex_destroy(&mutex->mutex);
        os_memory_release(mutex);
    }
}

b8 os_sys_mutex_try_lock (OS_Mutex* mutex)
{
    return pthread_mutex_trylock(&mutex->mutex) == 0;
}

void os_sys_mutex_lock (OS_Mutex* mutex)
{
    pthread_mutex_lock(&mutex->mutex);
}

void os_sys_mutex_unlock (OS_Mutex* mutex)
{
    pthread_mutex_unlock(&mutex->mutex);
}

OS_Semaphore* os_semaphore_create (u32 initial)
{
    OS_Semaphore* semaphore = (OS_Semaphore*)os_memory_alloc(sizeof(OS_Semaphore));
    pthread_mutex_init(&semaphore->mutex, nullptr);
    pthread_cond_init(&semaphore->cond, nullptr);
    semaphore->count = initial;
    return semaphore;
}

void os_semaphore_destroy (OS_Semaphore* semaphore)
{
    if (semaphore)
    {
        pthread_cond_destroy(&semaphore->cond);
        pthread_mutex_destroy(&semaphore->mutex);
        os_memory_release(semaphore);
    }
}

void os_semaphore_wait (OS_Semaphore* semaphore)
{
    pthread_mutex_lock(&semaphore->mutex);
    while (semaphore->count == 0)
    {
        pthread_cond_wait(&semaphore->cond, &semaphore->mutex);
    }
    semaphore->count--;
    pthread_mutex_unlock(&semaphore->mutex);
}

b8 os_sys_semaphore_wait_ns (OS_Semaphore* semaphore, u64 timeout_ns)
{
    const timespec deadline = deadline_after(timeout_ns);
    pthread_mutex_lock(&semaphore->mutex);
    while (semaphore->count == 0)
    {
        if (pthread_cond_timedwait(&semaphore->cond, &semaphore->mutex, &deadline) == ETIMEDOUT)
        {
            break;
        }
    }
    const b8 taken = semaphore->count > 0;
    if (taken)
    {
        semaphore->count--;
    }
    pthread_mutex_unlock(&semaphore->mutex);
    return taken;
}

b8 os_semaphore_try_wait (OS_Semaphore* semaphore)
{
    pthread_mutex_lock(&semaphore->mutex);
    const b8 taken = semaphore->count > 0;
    if (taken)
    {
        semaphore->count--;
    }
    pthread_mutex_unlock(&semaphore->mutex);
    return taken;
}

void os_semaphore_signal (OS_Semaphore* semaphore)
{
    pthread_mutex_lock(&semaphore->mutex);
    semaphore->count++;
    pthread_cond_signal(&semaphore->cond);
    pthread_mutex_unlock(&semaphore->mutex);
}

OS_Thread* os_thread_create (const char* name, OS_ThreadFunction* function, void* user)
{
    OS_Thread* thread = (OS_Thread*)os_memory_alloc(sizeof(OS_Thread));
    thread->function  = function;
    thread->user      = user;
    cstr_copy(thread->name, sizeof(thread->name), name ? name : "");
    if (pthread_create(&thread->thread, nullptr, thread_trampoline, thread) != 0)
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
    pthread_join(thread->thread, nullptr);
    const i32 result = thread->result;
    os_memory_release(thread);
    return result;
}

u64 os_thread_current_id ()
{
    return (u64)(uintptr_t)pthread_self();
}

void os_sys_thread_name (char* out_name, usize size)
{
    out_name[0] = 0;
    pthread_getname_np(pthread_self(), out_name, size);
}

//
// Logging, debugging and output capture
//

void os_sys_log_write (OS_LogLevel level, const char* text)
{
    static const char* const prefixes[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    (void)fprintf(stderr, "%s: %s\n", prefixes[(u32)level], text);
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
#else
    return false;
#endif
}

void os_trigger_breakpoint ()
{
    if (debugger_attached())
    {
        (void)raise(SIGTRAP);
    }
}

b8 os_default_audio_device (b8 input, wchar_t* out_id, usize count)
{
    (void)input;
    if (out_id && count > 0)
    {
        out_id[0] = 0;
    }
    return false;
}

b8 os_sys_pipe (i32 out_fds[2])
{
    if (pipe(out_fds) != 0)
    {
        return false;
    }
    fcntl(out_fds[0], F_SETFL, fcntl(out_fds[0], F_GETFL) | O_NONBLOCK);
    return true;
}

i32 os_sys_fd_dup (i32 fd)
{
    return dup(fd);
}

void os_sys_fd_dup2 (i32 from, i32 to)
{
    (void)dup2(from, to);
}

void os_sys_fd_close (i32 fd)
{
    (void)close(fd);
}

void os_sys_fd_write (i32 fd, const void* data, usize size)
{
    if (fd >= 0)
    {
        const ssize_t written = write(fd, data, size);
        UNUSED(written);
    }
}

i64 os_sys_fd_read_available (i32 fd, void* data, usize capacity)
{
    return read(fd, data, capacity);
}

#if HAVE_LINUX
//
// Window thread wakeup
//

void os_sys_wake_open ()
{
    if (pipe(os_wake_fds) != 0)
    {
        os_wake_fds[0] = -1;
        os_wake_fds[1] = -1;
        return;
    }
    for (const i32 fd : os_wake_fds)
    {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
}

void os_sys_wake_close ()
{
    for (i32& fd : os_wake_fds)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        fd = -1;
    }
}

void os_sys_wake_signal ()
{
    const u8      byte    = 1;
    const ssize_t written = os_wake_fds[1] >= 0 ? write(os_wake_fds[1], &byte, 1) : 0;
    UNUSED(written); // a full pipe is set already
}

void os_sys_wake_clear ()
{
    u8 buffer[64];
    while (os_wake_fds[0] >= 0 && read(os_wake_fds[0], buffer, sizeof(buffer)) > 0)
    {
    }
}

void os_sys_wake_wait (u64 timeout_ns)
{
    pollfd entry = { .fd = os_wake_fds[0], .events = POLLIN, .revents = 0 };
    poll(&entry, 1, (int)(timeout_ns / 1000000ull));
    os_sys_wake_clear();
}

intptr_t os_sys_wake_handle ()
{
    return os_wake_fds[0];
}
#endif

//
// Directories, sockets and lock files
//

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
