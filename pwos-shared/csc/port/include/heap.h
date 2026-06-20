#ifndef PWOS_CSC_HEAP_H
#define PWOS_CSC_HEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * lttit CSC 依赖 heap_malloc/heap_free。
 *
 * PC 测试版用 malloc/free；STM32 版应替换为固定池或 FreeRTOS heap，
 * 并避免在 ISR/热路径调用动态分配。
 */
void *heap_malloc_dbg(size_t size, const char *file, int line);
void heap_free_dbg(void *ptr, const char *file, int line);

#define heap_malloc(size) heap_malloc_dbg((size), NULL, 0)
#define heap_free(ptr) heap_free_dbg((ptr), NULL, 0)

#ifdef __cplusplus
}
#endif

#endif
