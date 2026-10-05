/*
 * notify.c —— 跨线程「有变更」通知（真实实现）
 *
 * 全局版本号 + 条件变量。锁实现与 transfer.c 保持同一套路：
 * Windows 懒初始化临界区 + 条件变量；POSIX 静态互斥量 + 条件变量。
 *
 * 唤醒采用「广播 + 版本号复核」：等待者醒来后重新取数，取不到就再等。
 * 某个用户的消息会顺带唤醒其他人（多一次空查询），规模小、可接受，
 * 相比为每个房间/用户维护独立的等待队列要简单得多，也少一个出错面。
 */
#include "notify.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>

static CRITICAL_SECTION   g_cs;
static CONDITION_VARIABLE g_cv;
static volatile LONG      g_init = 0;
#else
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond  = PTHREAD_COND_INITIALIZER;
#endif

static long g_ver = 0;

#ifdef _WIN32
/* 懒初始化（与 db.c / transfer.c 相同的双状态机，避免静态初始化顺序问题） */
static void notify_init(void)
{
    for (;;) {
        LONG st = InterlockedCompareExchange(&g_init, 1, 0);
        if (st == 0) {
            InitializeCriticalSection(&g_cs);
            InitializeConditionVariable(&g_cv);
            InterlockedExchange(&g_init, 2);
            return;
        }
        if (st == 2) {
            return;
        }
        Sleep(0);
    }
}
#endif

void notify_ping(void)
{
#ifdef _WIN32
    notify_init();
    EnterCriticalSection(&g_cs);
    g_ver++;
    WakeAllConditionVariable(&g_cv);
    LeaveCriticalSection(&g_cs);
#else
    pthread_mutex_lock(&g_mutex);
    g_ver++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
#endif
}

long notify_ver(void)
{
    long v;

#ifdef _WIN32
    notify_init();
    EnterCriticalSection(&g_cs);
    v = g_ver;
    LeaveCriticalSection(&g_cs);
#else
    pthread_mutex_lock(&g_mutex);
    v = g_ver;
    pthread_mutex_unlock(&g_mutex);
#endif
    return v;
}

void notify_wait_until(long ver, int timeout_ms)
{
    if (timeout_ms <= 0) {
        return;
    }

#ifdef _WIN32
    {
        ULONGLONG deadline;

        notify_init();
        EnterCriticalSection(&g_cs);
        deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
        while (g_ver == ver) {
            ULONGLONG now = GetTickCount64();
            DWORD step;

            if (now >= deadline) {
                break;
            }
            step = (DWORD)((deadline - now) > 1000 ? 1000 : (deadline - now));
            /* 返回 FALSE 可能是超时也可能是被唤醒，交给循环条件用版本号判断 */
            SleepConditionVariableCS(&g_cv, &g_cs, step);
        }
        LeaveCriticalSection(&g_cs);
    }
#else
    {
        struct timespec ts;
        struct timeval tv;

        gettimeofday(&tv, NULL);
        ts.tv_sec  = tv.tv_sec + timeout_ms / 1000;
        ts.tv_nsec = (tv.tv_usec + (long)(timeout_ms % 1000) * 1000) * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&g_mutex);
        while (g_ver == ver) {
            if (pthread_cond_timedwait(&g_cond, &g_mutex, &ts) == ETIMEDOUT) {
                break;
            }
        }
        pthread_mutex_unlock(&g_mutex);
    }
#endif
}
