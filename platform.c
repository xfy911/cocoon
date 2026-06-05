/**
 * platform.c - 跨平台抽象层实现
 *
 * 高性能实现原则：
 *   - sendfile 替代方案使用 64KB 大块缓冲区，最大化单次 I/O 吞吐量
 *   - Windows 目录遍历用 FindFirstFile 一次性获取，减少系统调用次数
 *   - 错误码统一映射，避免跨平台条件分支污染业务代码
 *   - socket 关闭统一用 cocoon_socket_close，防止 Windows 下误用 close()
 *
 * @author xfy
 */

#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ========== POSIX 实现 ========== */
#ifdef COCOON_PLATFORM_POSIX

#include <fcntl.h>      /* O_NONBLOCK */
#include <unistd.h>     /* close, read, write, lseek, dup2 */
#include <dirent.h>     /* opendir, readdir, closedir */
#include <errno.h>      /* errno, EAGAIN, EINTR */
#include <sys/sendfile.h> /* sendfile */
#include <signal.h>     /* sigaction */
#include <sys/socket.h> /* send */

/**
 * POSIX socket 子系统无需显式初始化。
 */
int cocoon_socket_init(void) { return 0; }

/**
 * POSIX socket 子系统无需显式清理。
 */
void cocoon_socket_cleanup(void) { }

/**
 * POSIX 下使用 fcntl 设置 O_NONBLOCK 标志。
 */
int cocoon_socket_nonblock(cocoon_socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * POSIX 下 socket 与文件描述符统一用 close()。
 */
int cocoon_socket_close(cocoon_socket_t fd) {
    return close(fd);
}

/**
 * POSIX 下使用 shutdown(SHUT_RDWR) 双向断开连接。
 */
int cocoon_socket_shutdown(cocoon_socket_t fd) {
    return shutdown(fd, SHUT_RDWR);
}

/**
 * POSIX 下 socket 发送直接用 write()，
 * 内核态零拷贝路径，无需额外封装。
 */
ssize_t cocoon_socket_send(cocoon_socket_t fd, const char *buf, size_t len) {
    return write(fd, buf, len);
}

ssize_t cocoon_socket_recv(cocoon_socket_t fd, void *buf, size_t len) {
    return read(fd, buf, len);
}

/**
 * POSIX 下文件操作直接映射标准 API。
 */
cocoon_file_t cocoon_file_open(const char *path) {
    return open(path, O_RDONLY);
}

ssize_t cocoon_file_read(cocoon_file_t fd, void *buf, size_t count) {
    return read(fd, buf, count);
}

int64_t cocoon_file_seek(cocoon_file_t fd, int64_t offset, int whence) {
    return lseek(fd, (off_t)offset, whence);
}

int cocoon_file_close(cocoon_file_t fd) {
    return close(fd);
}

int cocoon_file_stat(const char *path, cocoon_stat_t *st) {
    return stat(path, st);
}

int cocoon_file_fstat(cocoon_file_t fd, cocoon_stat_t *st) {
    return fstat(fd, st);
}

bool cocoon_stat_isreg(const cocoon_stat_t *st) {
    return S_ISREG(st->st_mode);
}

bool cocoon_stat_isdir(const cocoon_stat_t *st) {
    return S_ISDIR(st->st_mode);
}

int64_t cocoon_stat_size(const cocoon_stat_t *st) {
    return (int64_t)st->st_size;
}

time_t cocoon_stat_mtime(const cocoon_stat_t *st) {
    return st->st_mtime;
}

/**
 * Linux 下优先使用 sendfile 零拷贝。
 * 失败时回退到 read+write 循环，64KB 缓冲区。
 */
ssize_t cocoon_file_send(cocoon_socket_t sock, cocoon_file_t file,
                         int64_t offset, size_t count) {
    off_t off = (off_t)offset;
    ssize_t sent = sendfile(sock, file, &off, count);
    if (sent >= 0 || (errno != EINVAL && errno != ENOSYS)) {
        return sent;
    }

    /* sendfile 不支持此场景，回退到 read+write */
    if (lseek(file, (off_t)offset, SEEK_SET) < 0) {
        return -1;
    }

    char buf[65536]; /* 64KB 缓冲区，最大化单次 I/O */
    size_t total = 0;
    while (total < count) {
        size_t to_read = count - total;
        if (to_read > sizeof(buf)) to_read = sizeof(buf);
        ssize_t n = read(file, buf, to_read);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return (total > 0) ? (ssize_t)total : -1;
        }

        size_t buf_sent = 0;
        while (buf_sent < (size_t)n) {
            ssize_t w = write(sock, buf + buf_sent, (size_t)n - buf_sent);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return (total > 0) ? (ssize_t)total : -1;
            }
            if (w == 0) return (total > 0) ? (ssize_t)total : -1;
            buf_sent += (size_t)w;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/**
 * POSIX 下目录遍历直接用 opendir/readdir/closedir。
 */
int cocoon_dir_open(cocoon_dir_iter_t *iter, const char *path) {
    if (!iter || !path) return -1;
    iter->dir = opendir(path);
    return (iter->dir != NULL) ? 0 : -1;
}

int cocoon_dir_next(cocoon_dir_iter_t *iter) {
    if (!iter || !iter->dir) return -1;
    struct dirent *entry = readdir(iter->dir);
    if (!entry) return 1; /* 遍历结束 */

    strncpy(iter->name, entry->d_name, sizeof(iter->name) - 1);
    iter->name[sizeof(iter->name) - 1] = '\0';

    /* 用 d_type 快速判断（如果支持），否则 stat */
#ifdef _DIRENT_HAVE_D_TYPE
    iter->is_dir = (entry->d_type == DT_DIR);
#else
    char full_path[4096];
    /* 注意：这里 path 未存储在 iter 中，调用方需自行判断 */
    iter->is_dir = false; /* 保守默认值，static.c 会 stat 验证 */
#endif
    return 0;
}

void cocoon_dir_close(cocoon_dir_iter_t *iter) {
    if (iter && iter->dir) {
        closedir(iter->dir);
        iter->dir = NULL;
    }
}

/**
 * POSIX 信号处理：SIGINT + SIGTERM。
 */
static void (*g_signal_handler)(int) = NULL;

static void posix_signal_handler(int sig) {
    if (g_signal_handler) g_signal_handler(sig);
}

void cocoon_signal_setup(void (*handler)(int)) {
    g_signal_handler = handler;
    signal(SIGINT, posix_signal_handler);
    signal(SIGTERM, posix_signal_handler);
}

/**
 * POSIX 下获取 CPU 核心数。
 */
uint32_t cocoon_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 4;
}

/**
 * POSIX 下路径规范化用 realpath。
 */
bool cocoon_realpath(const char *path, char *resolved, size_t resolved_size) {
    if (!path || !resolved || resolved_size == 0) return false;
    char *r = realpath(path, NULL);
    if (r) {
        strncpy(resolved, r, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        free(r);
        return true;
    }
    return false;
}

/**
 * POSIX 下创建目录。
 */
int cocoon_mkdir(const char *path) {
    return mkdir(path, 0755);
}

/**
 * POSIX 下直接返回 errno。
 */
int cocoon_get_last_error(void) {
    return errno;
}

const char *cocoon_strerror(int err) {
    return strerror(err);
}

#endif /* COCOON_PLATFORM_POSIX */


/* ========== Windows 实现 ========== */
#ifdef COCOON_PLATFORM_WINDOWS

#include <fcntl.h>  /* _O_RDONLY, _O_BINARY */

/**
 * Windows 下 Winsock 需要显式初始化和清理。
 * 使用静态引用计数，确保多线程安全（此处单线程 accept 足够）。
 */
static int g_wsa_ref = 0;

int cocoon_socket_init(void) {
    if (g_wsa_ref == 0) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return -1;
        }
    }
    g_wsa_ref++;
    return 0;
}

void cocoon_socket_cleanup(void) {
    if (g_wsa_ref > 0) {
        g_wsa_ref--;
        if (g_wsa_ref == 0) {
            WSACleanup();
        }
    }
}

/**
 * Windows 下用 ioctlsocket(FIONBIO) 设置非阻塞。
 */
int cocoon_socket_nonblock(cocoon_socket_t fd) {
    u_long mode = 1; /* 1 = 非阻塞 */
    return ioctlsocket(fd, FIONBIO, &mode);
}

/**
 * Windows 下 socket 必须用 closesocket()，不能用 close()。
 */
int cocoon_socket_close(cocoon_socket_t fd) {
    return closesocket(fd);
}

/**
 * Windows 下 shutdown 行为与 POSIX 一致。
 */
int cocoon_socket_shutdown(cocoon_socket_t fd) {
    return shutdown(fd, SD_BOTH);
}

/**
 * Windows 下 socket 发送用 send()，不能用 write()。
 */
ssize_t cocoon_socket_send(cocoon_socket_t fd, const char *buf, size_t len) {
    /* send() 的 len 参数是 int，大于 INT_MAX 时分批发送 */
    int chunk = (len > INT_MAX) ? INT_MAX : (int)len;
    int r = send(fd, buf, chunk, 0);
    if (r == SOCKET_ERROR) {
        WSASetLastError(WSAGetLastError()); /* 保持错误码 */
        return -1;
    }
    return (ssize_t)r;
}

ssize_t cocoon_socket_recv(cocoon_socket_t fd, void *buf, size_t len) {
    int chunk = (len > INT_MAX) ? INT_MAX : (int)len;
    int r = recv(fd, (char *)buf, chunk, 0);
    if (r == SOCKET_ERROR) {
        WSASetLastError(WSAGetLastError());
        return -1;
    }
    return (ssize_t)r;
}

/**
 * Windows 下文件操作用 _open/_read/_lseek/_close（二进制模式）。
 */
cocoon_file_t cocoon_file_open(const char *path) {
    return _open(path, _O_RDONLY | _O_BINARY);
}

ssize_t cocoon_file_read(cocoon_file_t fd, void *buf, size_t count) {
    int chunk = (count > INT_MAX) ? INT_MAX : (int)count;
    int r = _read(fd, buf, chunk);
    return (r < 0) ? -1 : (ssize_t)r;
}

int64_t cocoon_file_seek(cocoon_file_t fd, int64_t offset, int whence) {
    return _lseeki64(fd, offset, whence);
}

int cocoon_file_close(cocoon_file_t fd) {
    return _close(fd);
}

/**
 * Windows 下用 _stat64 获取文件信息，支持大于 2GB 的文件。
 */
int cocoon_file_stat(const char *path, cocoon_stat_t *st) {
    return _stat64(path, st);
}

int cocoon_file_fstat(cocoon_file_t fd, cocoon_stat_t *st) {
    return _fstat64(fd, st);
}

bool cocoon_stat_isreg(const cocoon_stat_t *st) {
    return S_ISREG(st->st_mode);
}

bool cocoon_stat_isdir(const cocoon_stat_t *st) {
    return S_ISDIR(st->st_mode);
}

int64_t cocoon_stat_size(const cocoon_stat_t *st) {
    return (int64_t)st->st_size;
}

time_t cocoon_stat_mtime(const cocoon_stat_t *st) {
    return st->st_mtime;
}

/**
 * Windows 下无原生 sendfile，使用 read+send 循环。
 * 64KB 缓冲区，最大化单次 I/O 吞吐量，减少用户态/内核态切换开销。
 */
ssize_t cocoon_file_send(cocoon_socket_t sock, cocoon_file_t file,
                         int64_t offset, size_t count) {
    if (_lseeki64(file, offset, SEEK_SET) < 0) {
        return -1;
    }

    char buf[65536]; /* 64KB：现代页表和磁盘 I/O 的最佳平衡点 */
    size_t total = 0;

    while (total < count) {
        size_t to_read = count - total;
        if (to_read > sizeof(buf)) to_read = sizeof(buf);

        int chunk = (to_read > INT_MAX) ? INT_MAX : (int)to_read;
        int n = _read(file, buf, chunk);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return (total > 0) ? (ssize_t)total : -1;
        }

        size_t buf_sent = 0;
        while (buf_sent < (size_t)n) {
            int to_send = (int)((size_t)n - buf_sent);
            int w = send(sock, buf + buf_sent, to_send, 0);
            if (w == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK || err == WSAEINTR) continue;
                errno = EIO; /* 映射为通用 I/O 错误 */
                return (total > 0) ? (ssize_t)total : -1;
            }
            if (w == 0) return (total > 0) ? (ssize_t)total : -1;
            buf_sent += (size_t)w;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/**
 * Windows 下目录遍历用 FindFirstFile/FindNextFile。
 * 一次性获取所有信息，减少系统调用次数。
 */
int cocoon_dir_open(cocoon_dir_iter_t *iter, const char *path) {
    if (!iter || !path) return -1;

    /* 构建搜索路径：path + \\* */
    size_t len = strlen(path);
    if (len >= sizeof(iter->path_buf) - 3) return -1;

    memcpy(iter->path_buf, path, len);
    iter->path_buf[len] = '\\';
    iter->path_buf[len + 1] = '*';
    iter->path_buf[len + 2] = '\0';

    iter->handle = FindFirstFileA(iter->path_buf, &iter->data);
    if (iter->handle == INVALID_HANDLE_VALUE) return -1;

    iter->first = true;
    return 0;
}

int cocoon_dir_next(cocoon_dir_iter_t *iter) {
    if (!iter || iter->handle == INVALID_HANDLE_VALUE) return -1;

    if (!iter->first) {
        if (!FindNextFileA(iter->handle, &iter->data)) {
            if (GetLastError() == ERROR_NO_MORE_FILES) return 1;
            return -1;
        }
    }
    iter->first = false;

    /* 跳过 . 和 .. */
    while (iter->data.cFileName[0] == '.') {
        if (iter->data.cFileName[1] == '\0') goto skip;
        if (iter->data.cFileName[1] == '.' && iter->data.cFileName[2] == '\0') goto skip;
        break;
    skip:
        if (!FindNextFileA(iter->handle, &iter->data)) {
            if (GetLastError() == ERROR_NO_MORE_FILES) return 1;
            return -1;
        }
    }

    strncpy(iter->name, iter->data.cFileName, sizeof(iter->name) - 1);
    iter->name[sizeof(iter->name) - 1] = '\0';
    iter->is_dir = (iter->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 0;
}

void cocoon_dir_close(cocoon_dir_iter_t *iter) {
    if (iter && iter->handle != INVALID_HANDLE_VALUE) {
        FindClose(iter->handle);
        iter->handle = INVALID_HANDLE_VALUE;
    }
}

/**
 * Windows 控制台事件处理（替代 POSIX 信号）。
 */
static void (*g_win_handler)(int) = NULL;

static BOOL WINAPI win_ctrl_handler(DWORD ctrl_type) {
    (void)ctrl_type;
    if (g_win_handler) g_win_handler(2); /* SIGINT = 2 */
    return TRUE;
}

void cocoon_signal_setup(void (*handler)(int)) {
    g_win_handler = handler;
    SetConsoleCtrlHandler(win_ctrl_handler, TRUE);
}

/**
 * Windows 下用 GetSystemInfo 获取 CPU 核心数。
 */
uint32_t cocoon_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0) ? si.dwNumberOfProcessors : 1;
}

/**
 * Windows 下路径规范化用 _fullpath。
 */
bool cocoon_realpath(const char *path, char *resolved, size_t resolved_size) {
    if (!path || !resolved || resolved_size == 0) return false;
    char *r = _fullpath(NULL, path, 0);
    if (r) {
        strncpy(resolved, r, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        free(r);
        return true;
    }
    return false;
}

/**
 * Windows 下创建目录。
 */
int cocoon_mkdir(const char *path) {
    return _mkdir(path);
}

/**
 * Windows 错误码统一映射：
 * WSAEWOULDBLOCK → EAGAIN
 * WSAECONNRESET  → ECONNRESET
 * 其他保持原样。
 */
int cocoon_get_last_error(void) {
    int err = WSAGetLastError();
    switch (err) {
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAEINTR:       return EINTR;
        case WSAECONNRESET:  return ECONNRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        default:             return err;
    }
}

/**
 * Windows 下错误描述用 FormatMessage 获取。
 * 注意：线程不安全，仅用于日志输出。
 */
static __thread char g_err_buf[256];

const char *cocoon_strerror(int err) {
    if (err == 0) return "成功";

    DWORD fm = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, (DWORD)err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        g_err_buf, sizeof(g_err_buf), NULL);

    if (fm == 0) {
        snprintf(g_err_buf, sizeof(g_err_buf), "未知错误码 %d", err);
    }

    /* 去除尾部换行符 */
    size_t len = strlen(g_err_buf);
    while (len > 0 && (g_err_buf[len - 1] == '\n' || g_err_buf[len - 1] == '\r')) {
        g_err_buf[--len] = '\0';
    }
    return g_err_buf;
}

#endif /* COCOON_PLATFORM_WINDOWS */
