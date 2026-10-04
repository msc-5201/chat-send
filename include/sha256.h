/*
 * sha256.h —— 密码哈希（契约文件，冻结后只读）
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>

/* 计算 data[0..len) 的 SHA-256，输出 64 个小写十六进制字符 + '\0'。
 * out 至少需要 65 字节。 */
void sha256_hex(const char *data, size_t len, char *out);

#endif /* SHA256_H */
