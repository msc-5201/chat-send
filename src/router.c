/*
 * router.c —— 路由分发
 *
 * 路由表：method + path 精确匹配 → 处理函数；
 * 路径命中但方法不符 → 405；/api/ 下未命中 → 404；
 * 其余路径按静态资源处理（/ → /index.html），非 GET 访问静态资源 → 405。
 */
#include "router.h"

#include "auth.h"
#include "dashboard.h"
#include "friend.h"
#include "http.h"
#include "message.h"
#include "transfer.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *method;
    const char *path;
    int (*handler)(const request_t *req, int fd);
} route_t;

static const route_t ROUTES[] = {
    { "POST", "/api/register",        auth_register    },
    { "POST", "/api/login",           auth_login       },
    { "POST", "/api/guest",           auth_guest       },
    { "POST", "/api/logout",          auth_logout      },
    { "GET",  "/api/me",              auth_me          },
    { "POST", "/api/handle",          auth_set_handle  },
    { "POST", "/api/password",        auth_set_password },
    { "GET",  "/api/dashboard",       dashboard_get    },
    { "GET",  "/api/friends",         friend_list      },
    { "POST", "/api/friends/add",     friend_add       },
    { "POST", "/api/friends/accept",  friend_accept    },
    { "POST", "/api/friends/reject",  friend_reject    },
    { "GET",  "/api/messages",        message_fetch    },
    { "POST", "/api/messages",        message_send     },
    { "GET",  "/api/poll",            message_poll     },
    { "POST", "/api/transfer/send",   transfer_send    },
    { "GET",  "/api/transfer/poll",   transfer_poll    }
};

#define ROUTE_COUNT (sizeof(ROUTES) / sizeof(ROUTES[0]))

/* 访问日志：只记录方法、路径与简短结果，绝不打印请求体与 Cookie（避免泄漏密码/token） */
static void access_log(const char *method, const char *path, const char *result)
{
    printf("[访问] %s %s -> %s\n", method, path, result);
}

/* 静态资源只接受 GET */
static int static_method_allowed(const request_t *req)
{
    return strcmp(req->method, "GET") == 0;
}

void router_dispatch(int fd, const request_t *req)
{
    size_t i;
    int path_matched = 0;
    const char *static_path;

    for (i = 0; i < ROUTE_COUNT; i++) {
        if (strcmp(ROUTES[i].path, req->path) != 0) {
            continue;
        }
        path_matched = 1;
        if (strcmp(ROUTES[i].method, req->method) == 0) {
            int rc = ROUTES[i].handler(req, fd);   /* 处理函数自己发送响应 */
            char mark[32];

            /* 只记处理函数的返回码（契约规定该返回值仅用于日志），不记响应体 */
            snprintf(mark, sizeof(mark), "接口 rc=%d", rc);
            access_log(req->method, req->path, mark);
            return;
        }
    }

    if (strncmp(req->path, "/api/", 5) == 0) {
        if (path_matched) {
            http_respond_json(fd, 405, "{\"ok\":false,\"error\":\"请求方法不允许\"}");
            access_log(req->method, req->path, "405 方法不允许");
        } else {
            http_respond_json(fd, 404, "{\"ok\":false,\"error\":\"接口不存在\"}");
            access_log(req->method, req->path, "404 接口不存在");
        }
        return;
    }

    if (!static_method_allowed(req)) {
        http_respond_json(fd, 405, "{\"ok\":false,\"error\":\"请求方法不允许\"}");
        access_log(req->method, req->path, "405 静态资源仅支持 GET");
        return;
    }

    static_path = (strcmp(req->path, "/") == 0) ? "/index.html" : req->path;
    if (http_serve_static(fd, static_path) == CHAT_OK) {
        access_log(req->method, req->path, "200 静态资源");
        return;
    }

    http_respond_text(fd, 404, "text/html; charset=utf-8",
                      "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                      "<title>404 Not Found</title></head><body>"
                      "<h1>404 Not Found</h1><p>请求的资源不存在。</p>"
                      "</body></html>");
    access_log(req->method, req->path, "404 静态资源不存在");
}
