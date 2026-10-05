/*
 * notify.h —— 跨线程「有变更」通知（长轮询的唤醒机制，契约文件）
 *
 * 服务器是每连接一线程；前端原先每秒空轮询 /api/poll 与 /api/transfer/poll。
 * 改为长轮询后，处理函数在「没有新数据」时阻塞在 notify_wait_until()，
 * 直到任何消息 / 好友关系 / 传输信令发生变化（notify_ping()）或超时。
 *
 * 实现是一个全局版本号 + 条件变量：
 *   - notify_ping()            版本号 +1 并唤醒所有等待者；
 *   - notify_ver()             读取当前版本号（「先取数、后等待」的竞态防护）；
 *   - notify_wait_until(ver,ms) 版本号仍等于 ver 时阻塞，最多 ms 毫秒。
 */
#ifndef NOTIFY_H
#define NOTIFY_H

#include "chat.h"

/* 发生任何「客户端关心的变化」时调用：新消息、好友关系变化、新传输信令 */
void notify_ping(void);

/* 当前版本号；轮询处理函数在取数前记下它，取数后若为空再进入等待 */
long notify_ver(void);

/* 阻塞直到版本号 != ver，或超过 timeout_ms 毫秒 */
void notify_wait_until(long ver, int timeout_ms);

#endif /* NOTIFY_H */
