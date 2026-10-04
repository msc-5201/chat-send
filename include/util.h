/*
 * util.h —— 基础工具（契约文件，冻结后只读）
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* 生成 bytes 个随机字节的十六进制串，out 至少 2*bytes+1 字节 */
void util_random_hex(int bytes, char *out);

/* 标识码：8 位数字，例如 "48210937"，out 至少 9 字节 */
void util_gen_uid(char *out, size_t n);

/* 身份码：user_XXXX 形式，用于添加好友，out 至少 10 字节 */
void util_gen_handle(char *out, size_t n);

/* 游客临时标识码：G-XXXX 形式，out 至少 7 字节 */
void util_gen_guest_uid(char *out, size_t n);

/* 会话 token：32 字节随机数转十六进制，out 至少 65 字节 */
void util_gen_token(char *out, size_t n);

/* 私聊房间名：按 id 数值排序生成 "dm:<小>:<大>"，两人算出的名字一致。
 * 成功返回 CHAT_OK，房间名超过 n 返回 CHAT_ERR_FULL */
int util_dm_room(int a, int b, char *out, size_t n);

/* 解析 "dm:<a>:<b>"，成功返回 CHAT_OK 并写回 a、b，否则返回 CHAT_ERR */
int util_parse_dm_room(const char *room, int *a, int *b);

/* URL 百分号解码（同时处理 '+' 为空格）；返回写入长度 */
int util_url_decode(const char *in, char *out, size_t n);

/* 校验：用户名 3-32 位，只允许 [A-Za-z0-9_]。合法返回 1，非法返回 0 */
int util_valid_username(const char *s);

/* 校验：身份码 3-32 位 [A-Za-z0-9_-]。合法返回 1，非法返回 0 */
int util_valid_handle(const char *s);

/* 校验：可安全入库并原样返回给客户端的文本。
 * 要求是合法 UTF-8，且不含 C0 控制字符（\t \n \r 除外）与 DEL。
 * 拒绝控制字符还有一个实际作用：JSON 转义会把它们放大 6 倍
 * （\u00XX），恶意的控制字符消息会撑爆响应缓冲。
 * 合法返回 1，非法返回 0。 */
int util_valid_text(const char *s);

/* 当前本地时间 "YYYY-MM-DD HH:MM:SS"，out 至少 20 字节 */
void util_now_str(char *out, size_t n);

/* 当前 Unix 时间戳（秒） */
long util_now_ts(void);

#endif /* UTIL_H */
