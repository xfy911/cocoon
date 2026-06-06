/**
 * platform.h - 跨平台抽象层头文件
 *
 * 封装 POSIX 与 Windows 的系统差异，提供统一的跨平台 API。
 * 所有公共 API 均使用中文注释，遵循 Doxygen 风格。
 *
 * 设计原则：
 *   1. 最小化系统调用次数（高性能）
 *   2. Windows 下优先使用原生 API，拒绝兼容性层带来的额外开销
 *   3. 零拷贝优先：sendfile 在 Windows 下用 TransmitFile 或大块 read+send 循环
 *   4. 错误码统一：POSIX errno 与 Windows WSAGetLastError() 映射为统一错误码
 *
 * @author xfy
 */

#ifndef COCOON_PLATFORM_H
#define COCOON_PLATFORM_H

/* ========== 平台检测与系统头文件 ========== */

#ifdef _WIN32
    #define COCOON_PLATFORM_WINDOWS

    /* 精简 Windows 头文件，减少编译时间 */
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>       /**< Winsock 2（必须在 windows.h 之前） */
    #include <ws2tcpip.h>       /**< IPv6 扩展 */
    #include <windows.h>        /**< Windows 核心 API */
    #include <io.h>             /**< _open/_read/_close 等 */
    #include <direct.h>         /**< _mkdir */
    #include <sys/types.h>
    #include <sys/stat.h>

    /* Windows 下缺失的 POSIX 宏定义 */
    #ifndef S_ISREG
        #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
    #endif
    #ifndef S_ISDIR
        #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
    #endif

    /* MinGW 已提供 dirent.h，MSVC 需要自行实现 */
    #ifdef __MINGW32__
        #include <dirent.h>
        #define COCOON_HAS_DIRENT
    #endif

#else
    #define COCOON_PLATFORM_POSIX

    #include <sys/socket.h>     /**< socket/bind/listen/accept */
    #include <netinet/in.h>     /**< sockaddr_in */
    #include <netinet/tcp.h>    /**< TCP_NODELAY */
    #include <arpa/inet.h>      /**< htons/ntohs */
    #include <unistd.h>         /**< close/read/write */
    #include <fcntl.h>          /**< fcntl/open */
    #include <signal.h>         /**< signal/sigaction */
    #include <dirent.h>         /**< opendir/readdir */
    #include <sys/sendfile.h>   /**< sendfile（Linux 零拷贝） */
    #define COCOON_HAS_DIRENT
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

/* ========== 跨平台类型定义 ========== */

#ifdef COCOON_PLATFORM_WINDOWS
    /**
     * cocoon_socket_t - 跨平台 socket 描述符类型
     *
     * Windows 下为 SOCKET（UINT_PTR），POSIX 下为 int。
     */
    typedef SOCKET cocoon_socket_t;
    #define COCOON_INVALID_SOCKET INVALID_SOCKET   /**< 无效 socket 值 */

    /**
     * cocoon_file_t - 跨平台文件描述符类型
     *
     * Windows 下通过 _open/_close 管理，仍为 int。
     */
    typedef int cocoon_file_t;

    /**
     * cocoon_socklen_t - socket 地址长度类型
     */
    typedef int cocoon_socklen_t;

#else
    typedef int cocoon_socket_t;
    #define COCOON_INVALID_SOCKET (-1)
    typedef int cocoon_file_t;
    typedef socklen_t cocoon_socklen_t;
#endif

/* ========== socket 生命周期 API ========== */

/**
 * cocoon_socket_init - 初始化 socket 子系统
 *
 * Windows 下调用 WSAStartup；POSIX 下无操作。
 *
 * @return 0 成功，-1 失败
 */
int cocoon_socket_init(void);

/**
 * cocoon_socket_cleanup - 清理 socket 子系统
 *
 * Windows 下调用 WSACleanup；POSIX 下无操作。
 */
void cocoon_socket_cleanup(void);

/**
 * cocoon_socket_nonblock - 设置 socket 为非阻塞模式
 *
 * POSIX: fcntl(fd, F_SETFL, O_NONBLOCK)
 * Windows: ioctlsocket(fd, FIONBIO, &mode)
 *
 * @param fd socket 描述符
 * @return 0 成功，-1 失败
 */
int cocoon_socket_nonblock(cocoon_socket_t fd);

/**
 * cocoon_socket_close - 关闭 socket
 *
 * POSIX: close(fd)
 * Windows: closesocket(fd)
 *
 * @param fd socket 描述符
 * @return 0 成功，-1 失败
 */
int cocoon_socket_close(cocoon_socket_t fd);

/**
 * cocoon_socket_shutdown - 双向关闭 socket 连接
 *
 * 用于唤醒阻塞在 coco_read 的协程，触发连接超时清理。
 *
 * @param fd socket 描述符
 * @return 0 成功，-1 失败
 */
int cocoon_socket_shutdown(cocoon_socket_t fd);

/**
 * cocoon_socket_send - 向 socket 发送数据（高性能）
 *
 * POSIX: write(fd, buf, len)
 * Windows: send(fd, buf, len, 0)
 *
 * 自动处理 EAGAIN/EWOULDBLOCK/EINTR，调用者应自行循环。
 *
 * @param fd socket 描述符
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 实际发送字节数，-1 错误（设置 errno）
 */
ssize_t cocoon_socket_send(cocoon_socket_t fd, const char *buf, size_t len);

/**
 * cocoon_socket_recv - 从 socket 接收数据
 *
 * POSIX: read(fd, buf, len)
 * Windows: recv(fd, buf, len, 0)
 *
 * @param fd socket 描述符
 * @param buf 接收缓冲区
 * @param len 最大接收长度
 * @return 实际接收字节数，0 表示对端关闭，-1 错误
 */
ssize_t cocoon_socket_recv(cocoon_socket_t fd, void *buf, size_t len);

/* ========== 文件操作 API ========== */

/* 跨平台 stat 类型定义：Windows 用 _stat64（64位文件大小），POSIX 用 struct stat */
#ifdef COCOON_PLATFORM_WINDOWS
    typedef struct _stat64 cocoon_stat_t;
#else
    typedef struct stat cocoon_stat_t;
#endif

/**
 * cocoon_file_open - 以只读模式打开文件
 *
 * @param path 文件路径
 * @return 文件描述符，-1 失败
 */
cocoon_file_t cocoon_file_open(const char *path);

/**
 * cocoon_file_read - 从文件读取数据
 *
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 读取长度
 * @return 实际读取字节数，0 表示 EOF，-1 错误
 */
ssize_t cocoon_file_read(cocoon_file_t fd, void *buf, size_t count);

/**
 * cocoon_file_seek - 移动文件指针
 *
 * @param fd 文件描述符
 * @param offset 偏移量
 * @param whence 起始位置（SEEK_SET/SEEK_CUR/SEEK_END）
 * @return 新的文件位置，-1 错误
 */
int64_t cocoon_file_seek(cocoon_file_t fd, int64_t offset, int whence);

/**
 * cocoon_file_close - 关闭文件
 *
 * @param fd 文件描述符
 * @return 0 成功，-1 失败
 */
int cocoon_file_close(cocoon_file_t fd);

/**
 * cocoon_file_stat - 获取文件元数据
 *
 * @param path 文件路径
 * @param st 输出 stat 结构体（POSIX struct stat / Windows struct _stat）
 * @return 0 成功，-1 失败
 */
int cocoon_file_stat(const char *path, cocoon_stat_t *st);

/**
 * cocoon_file_fstat - 通过文件描述符获取元数据
 *
 * @param fd 文件描述符
 * @param st 输出 stat 结构体
 * @return 0 成功，-1 失败
 */
int cocoon_file_fstat(cocoon_file_t fd, cocoon_stat_t *st);

/**
 * cocoon_stat_isreg - 判断是否为普通文件
 *
 * @param st stat 结构体指针
 * @return true 是普通文件
 */
bool cocoon_stat_isreg(const cocoon_stat_t *st);

/**
 * cocoon_stat_isdir - 判断是否为目录
 *
 * @param st stat 结构体指针
 * @return true 是目录
 */
bool cocoon_stat_isdir(const cocoon_stat_t *st);

/**
 * cocoon_stat_size - 获取文件大小
 *
 * @param st stat 结构体指针
 * @return 文件大小（字节）
 */
int64_t cocoon_stat_size(const cocoon_stat_t *st);

/**
 * cocoon_stat_mtime - 获取文件修改时间
 *
 * @param st stat 结构体指针
 * @return 修改时间戳（秒）
 */
time_t cocoon_stat_mtime(const cocoon_stat_t *st);

/* ========== 高性能文件发送（零拷贝替代） ========== */

/**
 * cocoon_file_send - 将文件内容发送到 socket
 *
 * Linux: 优先使用 sendfile 零拷贝（内核态传输，无用户态内存拷贝）
 * Windows/其他: 使用 read+send 循环，64KB 大块缓冲区，
 *                最大化单次系统调用吞吐量，减少用户态/内核态切换
 *
 * @param sock 目标 socket
 * @param file 源文件描述符
 * @param offset 文件起始偏移量
 * @param count 发送字节数
 * @return 实际发送字节数，-1 错误
 */
ssize_t cocoon_file_send(cocoon_socket_t sock, cocoon_file_t file,
                         int64_t offset, size_t count);

/* ========== 目录遍历 API ========== */

/**
 * cocoon_dir_iter_t - 目录遍历迭代器
 *
 * POSIX: 包装 DIR*，一次性 opendir。
 * Windows: 包装 FindFirstFile/FindNextFile，避免多次系统调用。
 */
typedef struct cocoon_dir_iter {
#ifdef COCOON_PLATFORM_WINDOWS
    HANDLE handle;              /**< FindFirstFile 句柄 */
    WIN32_FIND_DATAA data;      /**< 当前文件数据 */
    bool first;                 /**< 首次调用标记 */
    char path_buf[4096];        /**< 完整搜索路径（含通配符） */
#else
    DIR *dir;                   /**< POSIX 目录句柄 */
#endif
    char name[256];             /**< 当前文件名（输出） */
    bool is_dir;                /**< 当前项是否为目录 */
} cocoon_dir_iter_t;

/**
 * cocoon_dir_open - 打开目录进行遍历
 *
 * @param iter 迭代器结构体（由调用者分配）
 * @param path 目录路径
 * @return 0 成功，-1 失败
 */
int cocoon_dir_open(cocoon_dir_iter_t *iter, const char *path);

/**
 * cocoon_dir_next - 获取下一个目录项
 *
 * @param iter 迭代器
 * @return 0 成功，1 遍历结束，-1 错误
 */
int cocoon_dir_next(cocoon_dir_iter_t *iter);

/**
 * cocoon_dir_close - 关闭目录迭代器
 *
 * @param iter 迭代器
 */
void cocoon_dir_close(cocoon_dir_iter_t *iter);

/* ========== 信号处理 API ========== */

/**
 * cocoon_signal_setup - 注册优雅关闭信号/事件处理
 *
 * POSIX: signal(SIGINT/SIGTERM, handler)
 * Windows: SetConsoleCtrlHandler（Ctrl+C / Ctrl+Break / 关闭事件）
 *
 * @param handler 信号处理函数（POSIX 下 sig 参数为信号编号）
 */
void cocoon_signal_setup(void (*handler)(int));

/**
 * cocoon_socket_poll_readable - 等待 socket 可读或超时
 *
 * POSIX: poll() + POLLIN
 * Windows: select() + fd_set（readfds）
 *
 * @param fd socket 描述符
 * @param timeout_ms 超时毫秒（-1 表示无限等待）
 * @return 1 可读，0 超时，-1 错误
 */
int cocoon_socket_poll_readable(cocoon_socket_t fd, int timeout_ms);

/**
 * cocoon_cpu_count - 获取 CPU 逻辑核心数
 *
 * POSIX: sysconf(_SC_NPROCESSORS_ONLN)
 * Windows: GetSystemInfo → dwNumberOfProcessors
 *
 * @return 逻辑 CPU 核心数（至少返回 1）
 */
uint32_t cocoon_cpu_count(void);

/* ========== 路径处理 API ========== */

/**
 * cocoon_realpath - 获取绝对路径
 *
 * POSIX: realpath()
 * Windows: _fullpath()（MinGW）或 GetFullPathNameA()
 *
 * @param path 输入路径
 * @param resolved 输出缓冲区
 * @param resolved_size 缓冲区大小
 * @return true 成功，false 失败
 */
bool cocoon_realpath(const char *path, char *resolved, size_t resolved_size);

/**
 * cocoon_mkdir - 创建目录
 *
 * POSIX: mkdir(path, 0755)
 * Windows: _mkdir(path)（默认权限）
 *
 * @param path 目录路径
 * @return 0 成功，-1 失败
 */
int cocoon_mkdir(const char *path);

/* ========== 错误处理 API ========== */

/**
 * cocoon_get_last_error - 获取最后一次系统错误码
 *
 * 统一映射：
 *   - POSIX socket 错误：直接返回 errno
 *   - Windows socket 错误：WSAGetLastError() → 映射到 POSIX errno 等价码
 *   - 文件操作错误：直接返回 errno / GetLastError() 映射
 *
 * @return 错误码（与 POSIX errno 兼容）
 */
int cocoon_get_last_error(void);

/**
 * cocoon_strerror - 将错误码转为可读的字符串
 *
 * @param err 错误码
 * @return 错误描述字符串（线程不安全，仅用于日志输出）
 */
const char *cocoon_strerror(int err);

#endif /* COCOON_PLATFORM_H */
