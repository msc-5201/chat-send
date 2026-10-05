# 聊天室 —— 用 C 写的聊天网站

一个用 **纯 C** 实现的聊天网站：单进程 HTTP 服务器 + SQLite 持久化 + 原生 HTML/CSS/JS 前端，
通过**长轮询**实现聊天实时更新。除 SQLite 官方单文件外**不依赖任何第三方库**。
文件传输基于 **WebRTC 点对点**（服务器只转发信令，文件字节不经过服务器），
传输功能来源于 **DashBeam**；需二人同时在线，速度只取决于双方网速，由于仅为网页端，过高网速跑不满。

## 功能

### 账号与权限

- **注册 / 登录 / 登出**：用户名 + 密码，密码以「随机盐 + SHA‑256」存储，会话用 Cookie（`sid`）维持
- **注册两次密码校验**：注册时需输入两次密码；不一致会被前端拦下，服务端再用 `password2` 复核
- **修改密码**：在仪表盘内操作 —— 校验当前密码 → 换新随机盐重新哈希 →
踢掉该用户**其它设备**上的登录会话（当前设备保留）
- **游客模式**：一键进入，无需注册，可使用世界聊天；不能加好友、私聊、传文件、看仪表盘

### 身份标识

- **标识码**：注册时生成的 8 位数字，唯一，**不可修改**（用于他人辨认你）
- **身份码**：默认形如 `user_8421`，唯一，**可随时修改**，用于被他人添加为好友

### 好友

- 按对方身份码发送申请 → 对方同意 / 拒绝 → 成为好友
- **好友列表即时刷新**：对方同意的瞬间，申请人无需刷新页面即可看到新好友

### 聊天

- **世界聊天**（`#world`）：注册用户与游客共用的公共群聊
- **一对一私聊**（`dm:<小id>:<大id>`）：仅好友之间；服务端做四重越权校验，非参与方一律 `403`
- **长轮询实时更新**：无新数据时服务器挂起请求（最多 25 秒），有消息立即返回；
空闲时每通道约 2‑3 次请求 / 分钟（原先是每秒一次空轮询，即 60 次 / 分钟）
- **数据持久化**：账号、身份码、好友关系、聊天记录重启后全部保留

### 文件传输（WebRTC 点对点）

- 服务器只转发 `offer` / `answer` / `ice` 信令，**文件字节不经过服务器**
- 支持图片 / 视频缩略图、进度条、SHA‑256 完整性校验、下载与全屏预览
- 分片发送 `ArrayBuffer` 并按数据通道 `maxMessageSize` 动态收窄；
接收端校验「实收字节数 == 声明大小」后才标记完成

### 个人仪表盘

- 侧栏「仪表盘」入口，展示用户名 / 数字 id / 标识码 / 身份码 / 注册时间 / 好友数 / 发言数
- 个人数据落盘为 `Dashboard/<用户名>/<用户名>.json`（注册时创建，打开仪表盘时刷新并自愈）
- 文件**只含非敏感个人数据**，绝不写入密码哈希、盐或会话 token

### 界面

- 原创「**墨玉 Ink Jade**」配色（青玉主色，非微信绿 / Telegram 蓝 / QQ 蓝）
- **深 / 浅双主题**：默认跟随系统，可手动切换并记忆
- 连续同侧消息自动聚合、聊天区点阵底纹、不对称气泡圆角
- 响应式：窄屏（≤ 760px）侧栏改为抽屉

## 改动记录

### 1. 界面改版（[public/style.css](public/style.css) 整体重写）

- 深色蓝紫渐变 + 玻璃拟态 → 「墨玉」原创青玉配色，扁平化、主流 IM 式布局
- 新增深浅双主题：右上角切换按钮 + `localStorage` 记忆 + 首帧防闪烁内联脚本
- 新增三个原创细节：连续同侧消息自动聚合（纯 CSS，零 JS 改动）、聊天区点阵底纹、不对称气泡圆角
- 所有承载白色文字的填充色均按 WCAG AA 核算（对白字 ≥ 4.5:1）
- 去掉 `color‑mix()`（需 Chrome 111+），改用显式 rgba 令牌以兼容更老的浏览器

### 2. 文件传输修复（图片 / 视频损坏、下载后 0KB）

- 分片由 `file.slice()` 的 **Blob** 改为 **ArrayBuffer**，并按 `channel.maxMessageSize ‑ 16` 动态收窄分片
- 修正末片长度计算（原 `offset += CHUNK` 会越界）
- 接收端新增「实收字节数 == 声明大小」完整性校验，避免截断 / 0 字节文件被标记为「已完成」

### 3. 聊天实时性改造（长轮询）

- 新增 [src/notify.c](src/notify.c) / [include/notify.h](include/notify.h)：全局版本号 + 条件变量
（Windows `CONDITION_VARIABLE` / POSIX `pthread_cond`）
- `/api/poll` 与 `/api/transfer/poll` 改为长轮询，等待上限 `CHAT_POLL_WAIT_MS`（默认 25 秒，可编译期覆盖）
- 前端改为长轮询 + `AbortController`：切房间 / 发消息时取消在途请求，过期响应按请求序号丢弃
- 顺带修复登出后传输轮询定时器空转的问题

### 4. 好友列表即时刷新

- 新增好友关系版本号 `db_friend_rev()`；`/api/poll` 响应新增 `friends_rev` 字段（游客恒为 0）
- 前端发现版本号变化即重新拉取好友列表，不再需要手动刷新页面

### 5. 仪表盘（新增）

- 新增 [include/dashboard.h](include/dashboard.h) / [src/dashboard.c](src/dashboard.c)，新增路由 `GET /api/dashboard`
- 落盘 `Dashboard/<用户名>/<用户名>.json`：先写 `.tmp` 再改名（原子替换），用独立互斥锁串行化写入
- 写盘前强制校验用户名只含 `[A‑Za‑z0‑9_]`，杜绝目录穿越；文件不含任何凭据
- 前端新增侧栏入口、仪表盘视图与个人数据展示

### 6. 注册两次密码 + 修改密码

- 新增路由 `POST /api/password`
- `/api/register` 支持 `password2`（可选；带上就必须与 `password` 一致，否则 `400`）
- 前端注册弹窗新增「确认密码」输入框；仪表盘内新增修改密码表单

### 7. 数据层与路由分层修正

- 把 `notify_ping()` 的调用从数据层（`db_friend.c` / `db_message.c`）上移到路由层（`friend.c` / `message.c`）
- 修复了按原 README 命令编译 `db_smoke` 时因未链接 `notify.c` 导致的链接失败

### 8. 导航高亮修复

- 抽出 `syncNavActive()` 统一同步「世界聊天 / 仪表盘」两个入口的高亮，修复二者同时点亮

### 9. 契约与测试同步

- [API.md](API.md)：补充 `GET /api/dashboard`、`POST /api/password`、注册 `password2`、长轮询与 `friends_rev` 说明
- [tests/api_smoke.ps1](tests/api_smoke.ps1)：端到端断言增至 **177 项**，
新增仪表盘落盘、修改密码、两次密码、`friends_rev` 等用例

## 快速开始

```
cd test_web
.\build.bat
.\server.exe
```

然后浏览器打开 [http://localhost:8080](http://localhost:8080)。

首次运行 `build.bat` 需要编译 SQLite（约几十秒），之后会复用 `build\sqlite3.o` 缓存。
服务器会在自己所在目录生成 `chat.db`（SQLite 数据库）与 `Dashboard\`（每个注册用户一个
`Dashboard\<用户名>\<用户名>.json` 个人数据文件）；`Ctrl+C` 停止服务器。

### 编译期可覆盖项

表格

| 宏 | 默认值 | 说明 |
| --- | --- | --- |
| `CHAT_PORT` | `8080` | 监听端口 |
| `CHAT_DB_PATH` | `"chat.db"` | 数据库文件路径 |
| `CHAT_STATIC_ROOT` | `"public"` | 静态资源根目录 |
| `CHAT_DASHBOARD_ROOT` | `"Dashboard"` | 仪表盘个人数据根目录 |

四个宏都是相对 `server.exe` 所在目录解析的（程序启动时会把工作目录切到 exe 目录），
所以从任何位置启动行为都一致。例如换端口编译：

```
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
    notify.h          长轮询唤醒：全局版本号 + 条件变量（notify_ping / notify_wait_until）
    db.h              数据层接口
    auth.h            鉴权路由接口
    dashboard.h       仪表盘接口（个人数据落盘 + GET /api/dashboard）
    friend.h          好友路由接口
    message.h         消息路由接口
    router.h          路由分发接口
  src/
    main.c            服务器入口：套接字、每连接一线程、生命周期
    router.c          路由表与分发（405/404、静态资源回退）
    http.c            HTTP/1.1 解析、响应构造、静态文件服务
    notify.c          长轮询唤醒实现（Windows 临界区 + 条件变量 / POSIX 互斥量 + 条件变量）
    json.c            JSON 转义与拼接
    db.c              SQLite 连接、建表、全局锁
    db_user.c         用户表查询、改密码、仪表盘统计
    db_session.c      会话表查询
    db_friend.c       好友申请与好友关系
    db_message.c      消息读写
    auth.c            注册/登录/游客/登出/me/改身份码/改密码
    dashboard.c       仪表盘：Dashboard/<用户名>/<用户名>.json 的原子写入
    friend.c          好友申请、同意、拒绝、列表
    message.c         世界聊天、私聊、轮询
    sha256.c          SHA‑256 实现
    util.c            工具函数
  public/
    index.html        单页应用外壳
    style.css         界面样式
    app.js            前端逻辑（长轮询、渲染、仪表盘、XSS 转义）
  Dashboard/          运行期生成：每个注册用户一个目录，内含 <用户名>.json 个人数据
  tests/
    db_smoke.c        数据层自测（71 项断言）
    api_smoke.ps1     端到端接口测试（177 项断言）
  third_party/
    sqlite3.c/.h      SQLite 官方 amalgamation（未修改）
  build.bat           一键编译
  API.md              HTTP 接口契约
```

## 运行测试

### 数据层自测

```
gcc -O2 -std=c11 -Wall -Wextra -Iinclude -Ithird_party -o build\db_smoke.exe tests\db_smoke.c src\db.c src\db_user.c src\db_session.c src\db_friend.c src\db_message.c src\json.c src\util.c build\sqlite3.o
.\build\db_smoke.exe
```

会在 `build\` 下生成临时数据库并打印每项 `PASS`/`FAIL`，全通过时退出码为 0。

### 端到端接口测试

```
powershell -ExecutionPolicy Bypass -File tests\api_smoke.ps1
```

脚本会自行在私有端口 8099 编译并启动服务器、跑完全部断言、重启验证持久化，最后清理现场。
覆盖游客流程、注册登录（含两次密码校验）、标识码 / 身份码、好友申请与同意、世界聊天、
私聊越权、轮询、仪表盘与数据落盘、修改密码、静态资源、404/405、非法输入、
控制字符与非法 UTF‑8 的拒绝，以及重启后数据持久化。
退出码 0 表示全部通过。

## 架构说明

### 并发模型

每个连接一个线程（Windows `CreateThread`，POSIX `pthread`），因此前端的长轮询连接不会互相阻塞。
线程以 detach 方式运行，不 join；每个连接设有接收超时，避免半开连接把线程永久挂住。

SQLite 用 `SQLITE_OPEN_FULLMUTEX` 打开，并在所有数据访问外加一把全局互斥锁
（`db_lock()` / `db_unlock()`，见 [db.h](include/db.h)）。

### 数据流

```
浏览器 app.js
   │  fetch (x‑www‑form‑urlencoded) + Cookie: sid
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

表格

| 房间 | 形式 | 说明 |
| --- | --- | --- |
| 世界聊天 | `#world` | 所有人（含游客）可读写 |
| 私聊 | `dm:<小id>:<大id>` | 两个数字 id 按**数值升序**拼接，因此 `dm:9:3` 与 `dm:3:9` 命中同一房间 |

私聊房间在服务端会做四重校验：必须是好友、必须有一方是自己、另一方不能是自己、游客不可私聊，
否则返回 `403`，防止越权读取他人会话。

## 安全设计

表格

| 风险 | 处理方式 |
| --- | --- |
| 密码泄漏 | 只存 `SHA‑256(盐 + 密码)`，每用户独立随机盐，不存明文 |
| 密码比较计时攻击 | 逐字节 XOR 累加比较，不提前返回 |
| 会话伪造 | 32 字节密码学随机数（Windows `rand_s()`）转 64 位十六进制 |
| SQL 注入 | 全部使用 `sqlite3_prepare_v2` + 参数绑定，无字符串拼接 SQL |
| XSS | 前端一律用 `textContent` / `escapeHtml()` 渲染用户内容 |
| 路径穿越 | 静态文件拒绝含 `..` 的路径 |
| 用户枚举 | 登录失败时「用户不存在」与「密码错误」返回同一文案 |
| 越权读私聊 | 见上文房间校验 |
| 改密码后旧会话残留 | 换新随机盐重新哈希，并删除该用户其它设备上的会话（当前这一台保留） |
| 仪表盘目录穿越 | 落盘路径由用户名拼出，写盘前**再强制校验**只含 `[A‑Za‑z0‑9_]`，杜绝 `..`、`/`、`\`、`:` |
| 仪表盘数据泄漏 | 落盘 JSON 只放非敏感个人数据，绝不写入 `pass_hash`、`salt` 或会话 token |
| 仪表盘文件写坏 | 先写同目录 `.tmp` 再改名（原子替换），中途失败不会留下半截 JSON |
| 缓冲区溢出 | 所有输入按 `chat.h` 中的长度上限截断或拒绝，拷贝均保证 `'\0'` 结尾 |
| 消息刷爆房间 | 拒绝控制字符（JSON 转义会把它们放大 6 倍），且取消息时按缓冲大小自动减少条数，宁可少给也绝不让房间整体报错 |
| 脏数据入库 | 消息正文校验为合法 UTF‑8，拒绝非法字节序列 / 过长编码 / 代理区 |
| 本机可移植性瑕疵 | 输入缓冲故意大于字段上限（如身份码用 128 字节），避免超长输入被截断后 "变合法" |

## 已知限制

- **实时性**：采用**长轮询**而非 WebSocket：`/api/poll` 与 `/api/transfer/poll` 在没有新数据时
由服务器挂起请求等待（`CHAT_POLL_WAIT_MS`，默认 25 秒），有变化立即返回（见 [notify.h](include/notify.h)）。
空闲时每个通道约 2‑3 次请求 / 分钟（原先 1 秒轮询是 60 次 / 分钟），新消息延迟接近 0。
代价：长轮询同样占用连接线程，线程数随在线人数线性增长（与原先相同）；在 C 里手写
WebSocket 的成本仍高于收益。
- **游客不持久**：游客没有 `users` 表记录，其会话在服务器重启时被清理（`db_session_clear_guests()`），
因此不能加好友、不能被加好友、不能私聊。
- **游客 id 跨重启复用**：游客数字 id 由递减计数器生成（‑1、‑2…），重启后会从头开始。
旧游客留下的历史消息可能被新游客的 id 撞上；前端对游客以**用户名**判定 "我的消息" 而非 id，因此显示不受影响。
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