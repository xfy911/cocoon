# Cocoon

一个基于 [coco](https://github.com/DefectingCat/coco) 协程库的轻量级静态资源 Web 服务器。

**Cocoon**（茧）—— 用协程的轻盈，包裹静态资源的安稳。

## 特性

- 🚀 **协程驱动并发** — 基于 coco 的有栈协程，每个连接一个协程，告别回调地狱
- 📁 **静态文件托管** — 支持目录列表、MIME 类型自动识别、范围请求（Range）
- 🧵 **多线程调度** — 利用 coco 的 M:N 调度器，自动扩展至多核
- ⚡ **零拷贝发送** — 支持 sendfile，减少用户态/内核态拷贝
- 🔧 **极简配置** — 命令行参数即可启动，无需配置文件

## 快速开始

### 构建

```bash
# 克隆仓库
git clone https://github.com/xfy911/cocoon.git
cd cocoon

# 构建（需要 coco 库和头文件）
make

# 运行
./cocoon -r ./examples/www -p 8080
```

### 依赖

- [coco](https://github.com/DefectingCat/coco) — 协程库
- Linux / macOS（支持 epoll / kqueue）

## 使用

```bash
# 基本用法
./cocoon -r /var/www/html -p 8080

# 多线程模式（自动检测 CPU 核心数）
./cocoon -r ./examples/www -p 8080 -t

# 指定工作线程数
./cocoon -r ./examples/www -p 8080 -w 8
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

## 性能

基于 coco 协程的高性能上下文切换（< 100ns），Cocoon 能够轻松处理数万并发连接。

## 许可证

MIT
