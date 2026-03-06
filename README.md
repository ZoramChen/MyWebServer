# TinyWebServer

这是一个运行于 Linux 的 C++ 轻量级 Web 服务器项目，整体采用 **非阻塞 socket + epoll + 线程池** 的并发架构，支持 **HTTP 静态资源**、**MySQL 注册/登录**。本仓库当前还集成了：

- **定时器**：清理非活跃连接（升序链表 + SIGALRM 驱动）
- **半同步/半反应堆线程池**：Reactor / Proactor（模拟）两种协作方式
- **同步/异步日志系统**：阻塞队列 + 后台线程（可选）
- **线程同步机制包装类**：mutex / semaphore / condition
- **Session ID 管理**：Cookie + 服务端内存（含过期、指纹校验）
- **SSL/TLS**：OpenSSL（TLS 1.2/1.3）
- **文件上传/下载**：支持分块上传、按用户隔离目录、下载列表 JSON

本 README 只基于本仓库现有代码结构说明“每个部分在哪里、做什么、怎么串起来”。

## 目录结构（与代码对应）

```
.
├── main.cpp                  # 程序入口：解析命令行参数，启动 WebServer
├── config.h / config.cpp     # 命令行参数与运行配置
├── webserver.h / webserver.cpp
├── http/                     # HTTP 连接处理、请求解析、上传文件模块
├── timer/                    # 定时器：处理非活动连接（升序链表）
├── threadpool/               # 半同步/半反应堆线程池
├── lock/                     # 线程同步机制包装类（mutex/sem/cond）
├── log/                      # 同步/异步日志系统（阻塞队列）
├── session/                  # Session ID 生成/验证/生命周期管理
├── ssl/                      # SSL/TLS 上下文与 socket 封装（OpenSSL）
├── CGImysql/                 # MySQL 连接池（RAII）
├── root/                     # 静态资源（html/css/js、上传目录等）
├── makefile / build.sh       # 构建
└── test_pressure/            # 压测相关（webbench）
```

## 核心流程（服务器从启动到处理请求）

- **入口**：`main.cpp`
  - 创建 `Config` 并解析命令行参数
  - 初始化 `WebServer`（端口、日志、线程池、数据库、触发模式）
  - 可选初始化 **SSL/TLS**（`-s 1` 开启）
  - 进入 `WebServer::eventLoop()` 事件循环
- **事件循环**：`webserver.h` / `webserver.cpp`
  - `eventListen()` 创建监听 socket，初始化 epoll，创建 `socketpair` 管道，用 `SIGALRM` 驱动定时器
  - `eventLoop()` 中 `epoll_wait()` 监听：
    - 新连接：`dealclientdata()` -> `timer()` 初始化连接对象与定时器
    - 可读：`dealwithread()` 将任务投递到线程池（Reactor）或主线程先读再投递（Proactor）
    - 可写：`dealwithwrite()` 发送响应
    - 超时信号：触发 `Utils::timer_handler()` -> `sort_timer_lst::tick()`

## 模块说明

### 定时器（处理非活动连接）

- **位置**：`timer/lst_timer.h`、`timer/lst_timer.cpp`
- **核心结构**：
  - `util_timer`：保存 `expire`、回调 `cb_func`、以及对应连接 `client_data`
  - `sort_timer_lst`：升序双向链表，接口 `add_timer/adjust_timer/del_timer/tick`
  - `Utils`：把 `SIGALRM` 写入管道，事件循环读到后调用 `timer_handler()` 驱动 `tick()`
- **与主流程关系**：
  - 新连接建立时：`WebServer::timer()` 创建 `util_timer` 并加入链表
  - 连接活跃时：`WebServer::adjust_timer()` 续期
  - 超时：`tick()` 触发回调关闭连接、清理资源

### 半同步/半反应堆线程池

- **位置**：`threadpool/threadpool.h`
- **核心点**：
  - `threadpool<T>` 维护任务队列 `m_workqueue`，用 `locker` 互斥 + `sem` 唤醒工作线程
  - `actor_model` 支持两种工作模式：
    - **Reactor**：工作线程负责 `read_once()/write()` + `process()`
    - **Proactor（模拟）**：主线程先读（`read_once()`），工作线程仅执行 `process()`
- **与 HTTP 的关系**：投递对象为 `http_conn`*，业务处理集中在 `http_conn::process()`

### 异步日志系统（同步/异步可选）

- **位置**：`log/log.h`、`log/log.cpp`、`log/block_queue.h`
- **核心点**：
  - `Log::init()`：当 `max_queue_size >= 1` 时启用异步写（后台线程消费阻塞队列）
  - `LOG_INFO/LOG_ERROR...`：宏统一入口（受 `global_close_log` 控制）
  - 按日期/行数切分日志文件（见 `log.cpp`）

### 线程同步机制包装类

- **位置**：`lock/locker.h`
- **提供封装**：
  - `locker`（pthread mutex）
  - `sem`（POSIX semaphore）
  - `cond`（pthread condition）
- **用途**：多线程同步，确保对共享资源的访问线程安全，防止出现竟态条件

### HTTP 连接处理类（请求解析/状态机/响应）

- **位置**：`http/http_conn.h`、`http/http_conn.cpp`
- **核心点**：
  - 主从状态机解析请求行/请求头/请求体（`process_read()`）
  - `do_request()` 根据 URL/标志位路由到静态资源、登录注册、上传下载等逻辑
  - `write()` 构造并发送响应；启用 SSL 时走 `SSLWrapper` 的 read/write

### Session ID 管理

- **位置**：`session/session.h`、`session/session_info.h`，并在 `http/http_conn.`* 中接入
- **实现要点**：
  - `SessionManager`：单例，维护 `session_id -> enhanced_session_info`，支持创建/验证/销毁/过期清理
  - `enhanced_session_info`：记录 `client_ip/client_port/user_agent`、过期时间（默认 30 分钟）
  - `http_conn::parse_headers()` 解析 `Cookie` 并提取 `session_id`
  - `http_conn::do_request()` 在访问受保护资源前验证 session；失败时跳转 `logTimeout.html`

### SSL/TLS 协议应用

- **位置**：`ssl/ssl_context.h`、`ssl/ssl_wrapper.h`、`ssl/ssl_wrapper.cpp`
- **实现要点**：
  - `OpenSSLContext`：初始化 OpenSSL、加载证书/私钥，最低 TLS 1.2、最高 TLS 1.3
  - `SSLWrapper`：封装 `SSL_accept/SSL_read/SSL_write/SSL_shutdown`
  - `WebServer::dealclientdata()`：accept 后为连接创建 `SSLWrapper` 并进行握手
- **注意**：
  - `ssl_context.h` 会校验私钥文件权限必须是 `0600`
  - 链接参数见 `makefile`：`-lssl -lcrypto`

### 文件上传/下载

- **页面**：`root/welcome.html`、`root/upload.html`、`root/download.html`
- **上传（分块）**：
  - **位置**：`http/upload_file.h/.cpp` + `http/http_conn.cpp`
  - 客户端以 `POST /9<filename>` 发送二进制块，并携带头：
    - `X-Chunk-Number`、`X-Total-Chunks`、`X-File-Name`、`X-File-Size`
  - 服务器端在 `http_conn::parse_headers()` 解析这些头；在 `do_request()` 的 `'9'` 分支调用：
    - `UploadFile::save_uploaded_chunk()` 保存到 `root/uploads_chunks/`
    - 最后一块到达时 `UploadFile::merge_uploaded_file()` 合并到 `root/uploads/<username>/<filename>`
- **下载（列表 + 文件）**：
  - 列表：`GET /b`（`do_request()` 的 `'b'` 分支）读取 `root/uploads/<username>/`，返回 JSON 数组
  - 文件：`GET /c<filename>`（`do_request()` 的 `'c'` 分支）从用户目录定位文件并通过 mmap 发送

## 构建与运行

### 依赖

- **编译器**：`g++`
- **库**：pthread、MySQL client、OpenSSL（见 `makefile`）

### 构建

```bash
sh ./build.sh
```

### 运行参数（`config.cpp` 解析）

```bash
./server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-q sql_num] [-t thread_num] [-c close_log] [-a actor_model] [-s use_ssl]
```

- **-p**：端口（默认 9006）
- **-l**：日志写入方式（0 同步 / 1 异步）
- **-m**：触发模式组合（0 LT+LT / 1 LT+ET / 2 ET+LT / 3 ET+ET）
- **-o**：优雅关闭（0 关闭 / 1 开启）
- **-q**：数据库连接池数量（默认 8）
- **-t**：线程池线程数（默认 8）
- **-c**：关闭日志（0 打开 / 1 关闭）
- **-a**：并发模型（0 Proactor / 1 Reactor）
- **-s**：是否启用 SSL/TLS（0 关闭 / 1 开启）

示例：

```bash
./server -p 9006 -l 1 -m 0 -o 0 -q 8 -t 8 -c 0 -a 0 -s 0
```

### 生成私钥和证书

```bash
# 生成私钥
openssl genrsa -out server.pem 2048
# 生成证书
openssl req -new -x509 -sha256 -key server.pem -out cert.pem -days 3650
```

### 致谢
Linux高性能服务器编程，游双著.

感谢qinguoyi提供的[TinyWebServer](https://github.com/qinguoyi/TinyWebServer)
