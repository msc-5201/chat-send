/*
 * main.c —— 服务器入口
 *
 * 初始化顺序：WSAStartup → 数据库 → 监听套接字 → accept 循环 → 退出清理。
 * 每连接一个独立线程（Windows: CreateThread，POSIX: pthread），
 * 前端每秒轮询也不会互相阻塞。
 */
#include "chat.h"
#include "db.h"
#include "http.h"
#include "router.h"
#include "sock.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
    #include <pthread.h>
    #include <sys/select.h>
#else
    #include <direct.h>
#endif

/* 单个连接的接收超时（秒）：防止半开连接把工作线程永久挂住 */
#define CONN_RECV_TIMEOUT_SEC 10

/* Ctrl+C 退出标志，信号处理函数只做这一件事 */
static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 把工作目录切到可执行文件所在目录。
 * 静态根目录 CHAT_STATIC_ROOT 与 CHAT_DB_PATH 都是相对路径，这样无论从哪个
 * 目录启动 server.exe，public/ 与 chat.db 都会相对 server.exe 解析，行为一致。 */
static void chdir_to_exe_dir(void)
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));

    if (n > 0 && n < (DWORD)sizeof(path)) {
        char *slash = strrchr(path, '\\');
        if (slash == NULL) {
            slash = strrchr(path, '/');
        }
        if (slash != NULL) {
            *slash = '\0';
            _chdir(path);
        }
    }
#else
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);

    if (n > 0) {
        char *slash;
        path[n] = '\0';
        slash = strrchr(path, '/');
        if (slash != NULL) {
            *slash = '\0';
            chdir(path);
        }
    }
#endif
}

/* 线程参数：用堆内存传递 SOCKET，避免按平台把整数强行转成指针 */
typedef struct {
    SOCKET client;
} conn_arg_t;

static void set_recv_timeout(SOCKET s, int seconds)
{
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, (int)sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, (socklen_t)sizeof(tv));
#endif
}

/* 在归属本线程的连接上完成「读取请求 → 分发」 */
static void handle_conn(SOCKET client)
{
    request_t req;

    set_recv_timeout(client, CONN_RECV_TIMEOUT_SEC);   /* 失败不影响主流程 */

    if (http_read_request((int)client, &req) == CHAT_OK) {
        router_dispatch((int)client, &req);
        return;
    }

    /* http_read_request 无法区分「报文非法」与「对端直接关闭」（都是 CHAT_ERR）。
     * 这里统一回 400；若对端已关闭，send 会立刻失败返回，不会阻塞线程。 */
    http_respond_json((int)client, 400, "{\"ok\":false,\"error\":\"请求格式错误\"}");
    printf("[访问] <无法解析的请求> -> 400 请求格式错误\n");
}

#ifdef _WIN32
static DWORD WINAPI conn_thread(LPVOID param)
#else
static void *conn_thread(void *param)
#endif
{
    conn_arg_t *arg = (conn_arg_t *)param;
    SOCKET client = arg->client;

    free(arg);   /* 参数所有权归本线程 */
    handle_conn(client);
    CHAT_CLOSE_SOCKET(client);

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* 创建分离线程；失败返回 CHAT_ERR，由调用方在当前线程兜底处理 */
static int start_conn_thread(SOCKET client)
{
    conn_arg_t *arg = (conn_arg_t *)malloc(sizeof(*arg));

    if (arg == NULL) {
        return CHAT_ERR;
    }
    arg->client = client;

#ifdef _WIN32
    {
        HANDLE h = CreateThread(NULL, 0, conn_thread, arg, 0, NULL);
        if (h == NULL) {
            free(arg);
            return CHAT_ERR;
        }
        CloseHandle(h);   /* detach：不 join，句柄立即释放 */
        return CHAT_OK;
    }
#else
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_thread, arg) != 0) {
            free(arg);
            return CHAT_ERR;
        }
        pthread_detach(tid);   /* detach：线程结束自动回收资源 */
        return CHAT_OK;
    }
#endif
}

int main(void)
{
    SOCKET server;
    struct sockaddr_in addr;
    int opt = 1;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    /* Windows CRT 会把 _IOLBF 当成全缓冲，日志被重定向时看不到内容；
     * 访问日志量很小，直接不缓冲，保证启动信息与访问日志立即落地。 */
    setvbuf(stdout, NULL, _IONBF, 0);

    chdir_to_exe_dir();

    if (db_open(CHAT_DB_PATH) != CHAT_OK) {
        fprintf(stderr, "警告：数据库打开失败，接口暂不可用\n");
    } else if (db_init_schema() != CHAT_OK) {
        fprintf(stderr, "警告：建表失败\n");
    } else {
        db_session_clear_guests();
    }

    signal(SIGINT, on_sigint);

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed\n");
        return 1;
    }

    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(CHAT_PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed on port %d\n", CHAT_PORT);
        CHAT_CLOSE_SOCKET(server);
        return 1;
    }

    if (listen(server, 32) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed\n");
        CHAT_CLOSE_SOCKET(server);
        return 1;
    }

    printf("聊天服务器已启动: http://localhost:%d\n", CHAT_PORT);
    printf("按 Ctrl+C 停止。\n");

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int sel;
        struct sockaddr_in peer;
        chat_socklen_t peer_len = sizeof(peer);
        SOCKET client;

        if (g_stop) {
            break;
        }

        /* 用带超时的 select 等待新连接：每 1 秒回到循环检查退出标志。
         * 这样即使在 Windows 上 accept 不会被信号打断，Ctrl+C 也能在 1 秒内退出。 */
        FD_ZERO(&rfds);
        FD_SET(server, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        sel = select((int)server + 1, &rfds, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) {
            if (g_stop) {
                break;
            }
            continue;
        }
        if (sel == 0) {
            continue;
        }

        client = accept(server, (struct sockaddr *)&peer, &peer_len);
        if (client == INVALID_SOCKET) {
            if (g_stop) {
                break;
            }
            continue;
        }

        if (start_conn_thread(client) != CHAT_OK) {
            /* 线程创建失败：退化为在当前线程处理，保证 socket 不被泄漏 */
            handle_conn(client);
            CHAT_CLOSE_SOCKET(client);
        }
    }

    printf("\n正在关闭服务器...\n");
    CHAT_CLOSE_SOCKET(server);
    /* 工作线程均为 detach，不 join；退出时由操作系统回收，
     * 少量在途请求可能被中断，属预期行为。 */
    db_close();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
