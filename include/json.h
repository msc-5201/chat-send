/*
 * json.h —— JSON 输出辅助（契约文件，冻结后只读）
 *
 * 所有 json_append_* 函数都以 out 当前内容的 strlen(out) 作为写入起点，
 * 在缓冲区尾部追加，返回追加的字符数；空间不足时返回 CHAT_ERR_FULL 且不改动 out。
 */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include "chat.h"

/* 把 in 转义成可以放进 JSON 字符串字面量的内容（不含首尾引号） */
void json_escape(const char *in, char *out, size_t n);

/* 追加 ,"key":"value" 或（当 out 为空串时）"key":"value" */
int json_append_str(char *out, size_t n, const char *key, const char *value);

/* 追加 ,"key":<number> */
int json_append_int(char *out, size_t n, const char *key, long value);

/* 追加 ,"key":true/false */
int json_append_bool(char *out, size_t n, const char *key, int value);

/* 追加一段已经构造好的原始 JSON 片段（不加引号） */
int json_append_raw(char *out, size_t n, const char *raw);

/* 在数组片段后面追加一个元素：out 非空时先补逗号，然后写入 raw。
 * 与 json_append_raw 的区别：空 out 时不会多写逗号，可直接用于拼接 JSON 数组。
 * 返回写入的字符数，空间不足返回 CHAT_ERR_FULL。 */
int json_array_push(char *out, size_t n, const char *raw);

#endif /* JSON_H */
