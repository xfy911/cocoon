# Cocoon

> 一只从 **coco** 协程库中孵化出的轻量 Web 服务器。

**Cocoon**（茧）—— 用协程的轻盈，包裹静态资源的安稳。

[中文](#特性) | [Quick Start](#quick-start)

---

## 特性

| 特性 | 描述 |
|------|------|
| 🚀 协程驱动 | 基于 coco 有栈协程，每个连接一个协程，上下文切换 < 100ns |
| 📁 静态托管 | 目录浏览、MIME 自动识别（25+ 种）、Range 请求、index.html 自动补全 |
| 🗜️ 动态压缩 | 对文本/JSON/CSS/JS 启用 Gzip 压缩，图片等二进制文件跳过 |
| 📦 缓存协商 | ETag + Last-Modified + If-None-Match + If-Modified-Since，自动 304 |
| 🧵 多核扩展 | M:N 调度器 + Work-stealing，自动负载均衡至多核 |
| ⚡ 零拷贝 | 优先使用 `sendfile`，减少用户态/内核态数据拷贝 |
| 🔐 路径安全 | 自动防护路径遍历攻击 (`../`) |
| 🛡️ 资源限制 | 空闲连接超时（默认 30s）+ 最大并发连接数限制 |
| 📝 分级日志 | error / warn / info / debug 四级输出，命令行可调 |
| 🌐 POST 回显 | 支持 JSON / form-urlencoded 请求体回显（API 测试） |
| 🔧 极简配置 | 纯命令行启动，无需配置文件 |

## Quick Start

### 依赖

- GCC / Clang（C11 标准）
- [coco](https://github.com/xfy911/coco) 协程库（通过 git submodule 自动获取）
- Linux（内核 ≥ 5.1，支持 io_uring）/ macOS
- CMake 3.10+（用于构建 coco 依赖）

### 构建

```bash
git clone --recursive https://github.com/xfy911/cocoon.git
cd cocoon

# 如果 clone 时忘了 --recursive，补初始化 submodule
git submodule update --init --recursive

# 构建 cocoon（会自动检测并构建 coco 依赖）
make build-all

# 或者分开构建
make deps   # 先构建 coco
make        # 再构建 cocoon

# 也可以使用系统上已有的 coco
make COCO_DIR=/path/to/coco
```

### 运行

```bash
# 单线程模式（开发调试）
./cocoon -r ./examples/www -p 8080

# 多线程模式（生产环境，自动检测 CPU 核心）
./cocoon -r ./examples/www -p 8080 -t

# 指定 8 个工作线程
./cocoon -r ./examples/www -p 8080 -t -w 8
```

### 测试

```bash
# 启动服务器
./cocoon -r ./examples/www -p 8080 &

# 访问首页
curl http://localhost:8080/

# 目录浏览
curl http://localhost:8080/examples/

# Range 请求
curl -H "Range: bytes=0-99" http://localhost:8080/index.html

# 缓存协商（304 Not Modified）
curl -I -H "If-None-Match: \"your-etag\"" http://localhost:8080/index.html

# Gzip 压缩
curl -I -H "Accept-Encoding: gzip" http://localhost:8080/index.html

# POST JSON 回显
curl -X POST -H "Content-Type: application/json" -d '{"hello":"world"}' http://localhost:8080/api/echo

# POST 表单回显
curl -X POST -H "Content-Type: application/x-www-form-urlencoded" -d 'name=cocoon' http://localhost:8080/api/echo
```

### 自动化测试

```bash
# 集成测试（curl + bash）
make test

# 性能基准（wrk）
make bench
```

## 命令行参数

```
Usage: ./cocoon [options]

Options:
  -r <dir>    静态资源根目录（必填）
  -p <port>   监听端口（默认 8080）
  -t          启用多线程调度
  -w <num>    工作线程数（默认自动检测 CPU 核心）
  -m <num>    最大并发连接数限制（默认 10000）
  -o <ms>     连接空闲超时毫秒数（默认 30000）
  -l <level>  日志级别：debug, info, warn, error（默认 info）
  -v          显示版本号
  -h          显示帮助
```

## 架构

```
┌─────────────┐     ┌─────────────────┐     ┌──────────────┐
│  main.c     │────▶│  server.c       │────▶│  static.c    │
│  (入口)      │     │  (TCP 服务器)    │     │  (静态文件)   │
└─────────────┘     └─────────────────┘     └──────────────┘
                            │
                            ▼
                     ┌─────────────────┐
                     │  http.c         │
                     │  (HTTP 解析)     │
                     └─────────────────┘
                            │
                            ▼
                     ┌─────────────────┐
                     │  coco 协程库    │
                     │  (并发 + I/O)   │
                     └─────────────────┘
```

### 模块职责

| 文件 | 职责 |
|------|------|
| `main.c` | 程序入口、信号处理、命令行参数解析 |
| `server.c` | TCP 服务器生命周期 + 连接超时管理 + 并发限制 |
| `http.c` | HTTP/1.1 请求解析、响应头格式化、MIME 类型推断 |
| `static.c` | 静态文件服务（sendfile/gzip）、目录浏览 HTML、错误响应 |
| `log.c` | 分级日志输出（error/warn/info/debug） |
| `cocoon.h` | 公共配置结构体与错误码定义 |

## 核心 API 速览

### HTTP 解析

```c
http_request_t req;
int parsed = http_parse_request(buf, buf_len, &req);
// req.method, req.path, req.content_length, req.keep_alive ...
```

### 响应格式化

```c
http_response_t resp = {
    .status_code = 200,
    .content_type = "text/html",
    .content_length = file_size,
    .keep_alive = true
};
int n = http_format_response_header(buf, sizeof(buf), &resp);
```

### 文件服务

```c
static_serve_file(fd, &req, "/var/www/html");
static_serve_directory(fd, &req, "/var/www/html", real_path);
static_send_error(fd, 404, true);
```

## 安全设计

- **路径遍历防护**：自动过滤 `../`，拒绝超出根目录的访问
- **目录隐藏**：不显示以 `.` 开头的隐藏文件
- **HTML 转义**：目录列表中的文件名自动转义，防止 XSS
- **请求方法过滤**：仅允许 `GET` / `HEAD`，拒绝其他方法

## 性能

| 指标 | 数值 | 条件 |
|------|------|------|
| 协程上下文切换 | < 100ns | coco 基准 |
| 单线程 RPS | ~16K | wrk, 100 连接, 4 线程 |
| 平均延迟 | ~60μs | wrk, 100 连接, 4 线程 |
| 最大并发 | 数万连接 | 单线程模式 |
| 多线程扩展 | 线性至核数 | M:N 调度 + Work-stealing |

> 压测环境：AMD EPYC / 4 核 / Ubuntu 22.04 / io_uring 后端

```bash
# 复现压测
make bench
```

## 示例页面

内置响应式示例首页，展示 Cocoon 三大特性：

![Cocoon Demo](examples/www/index.html)

> 渐变紫蓝背景 + 玻璃拟态卡片 + 移动端适配

## 路线图

- [x] ETag / Last-Modified 缓存协商 + 304 Not Modified
- [x] Gzip 动态压缩
- [x] 连接空闲超时 + 最大并发限制
- [x] 分级日志系统
- [x] POST 请求体解析（JSON / form-urlencoded 回显）
- [ ] HTTPS / TLS 支持
- [ ] HTTP/2 多路复用
- [ ] 配置文件支持（JSON / YAML）

## 构建与安装

```bash
make build-all    # 完整构建（含 coco 依赖）
make test         # 运行集成测试
make bench        # 运行性能基准
make install      # 安装到 /usr/local/bin
make clean        # 清理构建产物
```

## 许可证

MIT © xfy

> 用 [coco](https://github.com/DefectingCat/coco) 孵化，为轻量而生。
