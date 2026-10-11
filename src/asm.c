/* Functions the game has as hand-written assembly (copy and clear loops), in C for the PC port. */
#include "common.h"

#include <string.h>

void func_00189598(const void *src, void *dst, s32 words) { memcpy(dst, src, (size_t)words * 4); }
void func_001895B8(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 16); }
void func_001895F0(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 16); }
void func_00189618(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 16); }
void func_00189638(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 32); }
void func_00189690(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 32); }
void func_001896C8(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 32); }
void func_001896F0(const void *src, void *dst, s32 n) { memcpy(dst, src, (size_t)n * 64); }
void func_00189728(void *dst, s32 words) { memset(dst, 0, (size_t)words * 4); }
void func_00189740(void *dst, s32 qwords) { memset(dst, 0, (size_t)qwords * 16); }
