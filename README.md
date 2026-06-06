# Cocoon [![CI](https://github.com/xfy911/cocoon/actions/workflows/ci.yml/badge.svg)](https://github.com/xfy911/cocoon/actions/workflows/ci.yml)

> 一只从 **coco** 协程库中孵化出的轻量 Web 服务器。

**Cocoon**（茧）—— 用协程的轻盈，包裹静态资源的安稳。

[中文](#特性) | [Quick Start](#quick-start)

---

## 特性

| 特性 | 描述 |
|------|------|
| 🚀 协程驱动 | 基于 coco 有栈协程，每个连接一个协程，上下文切换 < 100ns |
| 📁 静态托管 | 目录浏览、MIME 自动识别（25+ 种）、Range 请求、index.html 自动补全 |
| 🗜️ 动态压缩 | 对文本/JSON/CSS/JS 启用 Brotli / Gzip 压缩，图片等二进制文件跳过，Brotli 优先 |
| 📦 缓存协商 | ETag + Last-Modified + If-None-Match + If-Modified-Since，自动 304 |
| 🧵 多核扩展 | M:N 调度器 + Work-stealing，自动负载均衡至多核 |
| ⚡ 零拷贝 | 优先使用 `sendfile`，减少用户态/内核态数据拷贝 |
| 🔐 路径安全 | 自动防护路径遍历攻击 (`../`) |
| 🛡️ 资源限制 | 空闲连接超时（默认 30s）+ 最大并发连接数限制 |
| 📝 分级日志 | error / warn / info / debug 四级输出，命令行可调 |
| 📊 访问日志 | Nginx combined 格式，记录 User-Agent / Referer / 状态码 |
| 🌐 POST 回显 | 支持 JSON / form-urlencoded / multipart 文件上传 |
| 🔧 配置文件 | JSON 配置文件 + 命令行参数覆盖，生产部署友好 |
| 🔒 HTTPS | TLS 1.2/1.3，OpenSSL Memory BIO 集成，ALPN 协商 HTTP/2 |
| 🚀 HTTP/2 | 完整 HTTP/2 支持（TLS + h2c 明文升级），多路复用 |
| 🔌 WebSocket | RFC 6455，支持广播 / 频道路由 / 定向发送 |
| 🧩 中间件 | 内置 CORS / Basic Auth / Rate Limit，可自定义注册 |
| 🔌 插件系统 | 动态加载 .so 插件，SIGUSR1 热重载 |
| 🏥 健康检查 | `/_health` 端点返回 JSON 服务器状态 |
| 🪟 跨平台 | Linux / macOS / Windows（MinGW/MSVC）|

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

# Brotli 压缩（优先）
curl -I -H "Accept-Encoding: br" http://localhost:8080/index.html

# Gzip 压缩（回退）
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

# 单元测试（Unity 框架）
make unit-test

# 一键运行所有测试
make test-all

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
  -v          详细日志输出（等同于 -l debug）
  --no-gzip   禁用 gzip 压缩
  --no-brotli 禁用 brotli 压缩
  --cors      启用 CORS 中间件
  --auth-user <user>  HTTP 基础认证用户名
  --auth-pass <pass>  HTTP 基础认证密码
  --rate-limit <rps>  每秒请求数限制（按 IP）
  --access-log <path> 访问日志路径（- 表示 stdout）
  --plugin <path>     加载插件（可多次指定）
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
| `static.c` | 静态文件服务（sendfile/gzip/brotli）、目录浏览 HTML、错误响应 |
| `log.c` | 分级日志输出（error/warn/info/debug） |
| `access_log.c` | 访问日志（Nginx combined 格式） |
| `config.c` | JSON 配置文件解析 |
| `multipart.c` | multipart/form-data 文件上传解析 |
| `tls.c` | TLS/SSL 层（OpenSSL Memory BIO） |
| `http2.c` | HTTP/2 协议实现（nghttp2） |
| `websocket.c` | WebSocket 协议（RFC 6455）+ 广播/频道路由 |
| `middleware.c` | 中间件注册表 + 内置 CORS / Basic Auth / Rate Limit |
| `plugin.c` | 动态插件加载 + 热重载 |
| `platform.c` | 跨平台抽象（Linux/macOS/Windows） |
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
- **请求方法过滤**：支持 `GET` / `HEAD` / `POST`，拒绝其他方法

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
- [x] Brotli / Gzip 动态压缩（Brotli 优先）
- [x] 连接空闲超时 + 最大并发限制
- [x] 分级日志系统 + 访问日志
- [x] POST 请求体解析（JSON / form-urlencoded / multipart 文件上传）
- [x] 配置文件支持（JSON）
- [x] HTTPS / TLS 支持
- [x] HTTP/2 多路复用（TLS + h2c）
- [x] WebSocket 支持（广播 + 频道）
- [x] 中间件机制（CORS / Basic Auth / Rate Limit）
- [x] 插件系统（动态加载 + 热重载）
- [x] 健康检查端点
- [x] 跨平台支持（Windows）
- [ ] 反向代理支持
- [ ] 虚拟主机 / 多站点

## 构建与安装

```bash
make build-all    # 完整构建（含 coco 依赖）
make test         # 运行集成测试
make unit-test    # 运行单元测试
make test-all     # 一键运行所有测试
make bench        # 运行性能基准
make install      # 安装到 /usr/local/bin
make clean        # 清理构建产物
```

## 许可证

MIT © xfy

> 用 [coco](https://github.com/DefectingCat/coco) 孵化，为轻量而生。
