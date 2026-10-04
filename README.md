# 聊天室 —— 用 C 写的聊天网站

一个用 **纯 C** 实现的聊天网站：单进程 HTTP 服务器 + SQLite 持久化 + 原生 HTML/CSS/JS 前端，
通过约 1 秒一次的轮询实现聊天实时更新。除 SQLite 官方单文件外**不依赖任何第三方库**。
传输功能来源于 **DashBeam**。 p2p点对点传输，需二人同时在线，传输速度仅依赖两者网速，由于仅为网页端，过高网速跑不满

## 功能

- **完整的登录与注册系统**：用户名 + 密码，密码以「随机盐 + SHA-256」存储，会话用 Cookie 维持
- **两种进入状态**
  - **游客模式**：一键进入，无需注册，可直接参与世界聊天
  - **登录模式**：解锁身份码、好友、私聊
- **标识码与身份码**
  - **标识码**：注册时生成的 8 位数字，唯一，**不可修改**（用于他人辨认你）
  - **身份码**：默认形如 `user_8421`，唯一，**可随时修改**，用于被他人添加为好友
- **好友系统**：按对方身份码发送申请 → 对方同意 → 成为好友（也支持拒绝）
- **世界聊天**：注册或游客进入后默认所在的公共群聊
- **一对一私聊**：仅好友之间可私聊
- **数据持久化**：账号、身份码、好友关系、聊天记录重启服务器后全部保留

## 快速开始

```powershell
cd test_web
.\build.bat
.\server.exe
```

然后浏览器打开 <http://localhost:8080>。

首次运行 `build.bat` 需要编译 SQLite（约几十秒），之后会复用 `build\sqlite3.o` 缓存。
服务器会在自己所在目录生成 `chat.db`（SQLite 数据库）；`Ctrl+C` 停止服务器。

### 编译期可覆盖项

| 宏 | 默认值 | 说明 |
|---|---|---|
| `CHAT_PORT` | `8080` | 监听端口 |
| `CHAT_DB_PATH` | `"chat.db"` | 数据库文件路径 |
| `CHAT_STATIC_ROOT` | `"public"` | 静态资源根目录 |

三个宏都是相对 `server.exe` 所在目录解析的（程序启动时会把工作目录切到 exe 目录），
所以从任何位置启动行为都一致。例如换端口编译：

```powershell
gcc -O2 -std=c11 -Iinclude -Ithird_party -DCHAT_PORT=9000 -o server.exe src\*.c build\sqlite3.o -lws2_32
```

## 目录结构

```
test_web/
  include/            契约头文件（模块间的冻结接口）
    chat.h            公共类型与常量
    sock.h            跨平台套接字屏蔽
    util.h            随机数/编解码/时间/字符串
    sha256.h          哈希接口
    json.h            JSON 构造与转义
    http.h            HTTP 解析/响应/静态文件
    db.h              数据层接口
    auth.h            鉴权路由接口
    friend.h          好友路由接口
    message.h         消息路由接口
    router.h          路由分发接口
  src/
    main.c            服务器入口：套接字、每连接一线程、生命周期
    router.c          路由表与分发（405/404、静态资源回退）
    http.c            HTTP/1.1 解析、响应构造、静态文件服务
    json.c            JSON 转义与拼接
    db.c              SQLite 连接、建表、全局锁
    db_user.c         用户表查询
    db_session.c      会话表查询
    db_friend.c       好友申请与好友关系
    db_message.c      消息读写
    auth.c            注册/登录/游客/登出/me/改身份码
    friend.c          好友申请、同意、拒绝、列表
    message.c         世界聊天、私聊、轮询
    sha256.c          SHA-256 实现
    util.c            工具函数
  public/
    index.html        单页应用外壳
    style.css         界面样式
    app.js            前端逻辑（轮询、渲染、XSS 转义）
  tests/
    db_smoke.c        数据层自测（71 项断言）
    api_smoke.ps1     端到端接口测试（129 项断言）
  third_party/
    sqlite3.c/.h      SQLite 官方 amalgamation（未修改）
  build.bat           一键编译
  API.md              HTTP 接口契约
```

## 运行测试

### 数据层自测

```powershell
gcc -O2 -std=c11 -Wall -Wextra -Iinclude -Ithird_party -o build\db_smoke.exe tests\db_smoke.c src\db.c src\db_user.c src\db_session.c src\db_friend.c src\db_message.c src\json.c src\util.c build\sqlite3.o
.\build\db_smoke.exe
```

会在 `build\` 下生成临时数据库并打印每项 `PASS`/`FAIL`，全通过时退出码为 0。

### 端到端接口测试

```powershell
powershell -ExecutionPolicy Bypass -File tests\api_smoke.ps1
```

脚本会自行在私有端口 8099 编译并启动服务器、跑完全部断言、重启验证持久化，最后清理现场。
覆盖游客流程、注册登录、标识码/身份码、好友申请与同意、世界聊天、私聊越权、轮询、
静态资源、404/405、非法输入、控制字符与非法 UTF-8 的拒绝，以及重启后数据持久化。
退出码 0 表示全部通过。

## 架构说明

### 并发模型

每个连接一个线程（Windows `CreateThread`，POSIX `pthread`），因此前端每秒轮询不会互相阻塞。
线程以 detach 方式运行，不 join；每个连接设有接收超时，避免半开连接把线程永久挂住。
SQLite 用 `SQLITE_OPEN_FULLMUTEX` 打开，并在所有数据访问外加一把全局互斥锁
（`db_lock()` / `db_unlock()`，见 [db.h](include/db.h)）。

### 数据流

```
浏览器 app.js
   │  fetch (x-www-form-urlencoded) + Cookie: sid
   ▼
src/http.c      解析请求 → request_t
   ▼
src/router.c    按 method + path 查表分发
   ▼
src/auth.c / friend.c / message.c      业务逻辑、会话校验
   ▼
src/db_*.c      SQLite 查询（参数绑定，全局锁保护）
```

响应统一为 JSON，静态资源由 `http_serve_static()` 从 `public/` 提供。

### 房间命名

| 房间 | 形式 | 说明 |
|---|---|---|
| 世界聊天 | `#world` | 所有人（含游客）可读写 |
| 私聊 | `dm:<小id>:<大id>` | 两个数字 id 按**数值升序**拼接，因此 `dm:9:3` 与 `dm:3:9` 命中同一房间 |

私聊房间在服务端会做四重校验：必须是好友、必须有一方是自己、另一方不能是自己、游客不可私聊，
否则返回 `403`，防止越权读取他人会话。

## 安全设计

| 风险 | 处理方式 |
|---|---|
| 密码泄漏 | 只存 `SHA-256(盐 + 密码)`，每用户独立随机盐，不存明文 |
| 密码比较计时攻击 | 逐字节 XOR 累加比较，不提前返回 |
| 会话伪造 | 32 字节密码学随机数（Windows `rand_s()`）转 64 位十六进制 |
| SQL 注入 | 全部使用 `sqlite3_prepare_v2` + 参数绑定，无字符串拼接 SQL |
| XSS | 前端一律用 `textContent` / `escapeHtml()` 渲染用户内容 |
| 路径穿越 | 静态文件拒绝含 `..` 的路径 |
| 用户枚举 | 登录失败时「用户不存在」与「密码错误」返回同一文案 |
| 越权读私聊 | 见上文房间校验 |
| 缓冲区溢出 | 所有输入按 `chat.h` 中的长度上限截断或拒绝，拷贝均保证 `'\0'` 结尾 |
| 消息刷爆房间 | 拒绝控制字符（JSON 转义会把它们放大 6 倍），且取消息时按缓冲大小自动减少条数，宁可少给也绝不让房间整体报错 |
| 脏数据入库 | 消息正文校验为合法 UTF-8，拒绝非法字节序列 / 过长编码 / 代理区 |
| 本机可移植性瑕疵 | 输入缓冲故意大于字段上限（如身份码用 128 字节），避免超长输入被截断后"变合法" |

## 已知限制

- **实时性**：采用 1 秒轮询而非 WebSocket。在 C 里手写 WebSocket 握手与帧解析成本过高，
  轮询已能满足聊天体验；代价是有最多 1 秒延迟且请求量随在线人数线性增长。
- **游客不持久**：游客没有 `users` 表记录，其会话在服务器重启时被清理（`db_session_clear_guests()`），
  因此不能加好友、不能被加好友、不能私聊。
- **游客 id 跨重启复用**：游客数字 id 由递减计数器生成（-1、-2…），重启后会从头开始。
  旧游客留下的历史消息可能被新游客的 id 撞上；前端对游客以**用户名**判定"我的消息"而非 id，因此显示不受影响。
- **单机部署**：全局 SQLite 连接 + 单进程内存状态，适合学习与小型自用，未考虑分布式部署。
- **会话不过期**：登出会删除会话，但会话本身没有自动过期策略。
- **POSIX 分支未经充分验证**：本项目以 Windows + MinGW 为目标（Winsock、`build.bat`），
  `sock.h` / `main.c` / `db.c` 保留了 POSIX 分支以便移植，但只在 Windows 上验证过。
  尤其注意 `util.c` 的随机数在非 Windows 平台会回退到 `rand()`（非线程安全、可预测），
  **若要部署到 Linux/macOS，请先把随机源换成 `/dev/urandom` 或 `getrandom()`**。
- **私聊历史的一次性可见范围**：进入房间时只取最近 100 条；消息特别长导致缓冲装不下时会先给最新的若干条，
  更早的消息不会被自动补回。

## 参考

- HTTP 接口契约见 [API.md](API.md)
- 数据库表结构见 [db.h](include/db.h) 顶部注释与 [db.c](src/db.c) 的 `db_init_schema()`
=======
