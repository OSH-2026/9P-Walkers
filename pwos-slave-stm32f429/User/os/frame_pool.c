#include "frame_pool.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static pwos_frame_block_t g_frame_pool[PWOS_FRAME_POOL_CAPACITY];
static pwos_frame_block_t *g_free_stack[PWOS_FRAME_POOL_CAPACITY];
static uint8_t g_in_use[PWOS_FRAME_POOL_CAPACITY];
static size_t g_free_count;
static uint32_t g_alloc_fail_count;
static uint32_t g_next_sequence;

static int frame_pool_index_of(const pwos_frame_block_t *block, size_t *out_index)
{
    size_t i;

    if (block == NULL) {
        return 0;
    }

    for (i = 0u; i < PWOS_FRAME_POOL_CAPACITY; ++i) {
        if (block == &g_frame_pool[i]) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return 1;
        }
    }
    return 0;
}

static pwos_frame_block_t *frame_pool_alloc_unlocked(void)
{
    pwos_frame_block_t *block;
    size_t index;

    if (g_free_count == 0u) {
        ++g_alloc_fail_count;
        return NULL;
    }

    block = g_free_stack[--g_free_count];
    if (!frame_pool_index_of(block, &index)) {
        ++g_alloc_fail_count;
        return NULL;
    }

    g_in_use[index] = 1u;
    block->port_id = 0u;
    block->reserved = 0u;
    block->len = 0u;
    block->timestamp_ms = 0u;
    block->sequence = ++g_next_sequence;
    return block;
}

static void frame_pool_free_unlocked(pwos_frame_block_t *block)
{
    size_t index;

    if (!frame_pool_index_of(block, &index)) {
        return;
    }
    if (g_in_use[index] == 0u) {
        return;
    }
    if (g_free_count >= PWOS_FRAME_POOL_CAPACITY) {
        return;
    }

    g_in_use[index] = 0u;
    block->len = 0u;
    g_free_stack[g_free_count++] = block;
}

void pwos_frame_pool_init(void)
{
    size_t i;

    memset(g_frame_pool, 0, sizeof(g_frame_pool));
    memset(g_in_use, 0, sizeof(g_in_use));
    g_free_count = 0u;
    g_alloc_fail_count = 0u;
    g_next_sequence = 0u;

    for (i = 0u; i < PWOS_FRAME_POOL_CAPACITY; ++i) {
        g_free_stack[g_free_count++] = &g_frame_pool[i];
    }
}

pwos_frame_block_t *pwos_frame_pool_alloc(void)
{
    pwos_frame_block_t *block;

    taskENTER_CRITICAL();
    block = frame_pool_alloc_unlocked();
    taskEXIT_CRITICAL();

    return block;
}

pwos_frame_block_t *pwos_frame_pool_alloc_from_isr(void)
{
    pwos_frame_block_t *block;
    UBaseType_t saved_mask;

    saved_mask = taskENTER_CRITICAL_FROM_ISR();
    block = frame_pool_alloc_unlocked();
    taskEXIT_CRITICAL_FROM_ISR(saved_mask);

    return block;
}

void pwos_frame_pool_free(pwos_frame_block_t *block)
{
    taskENTER_CRITICAL();
    frame_pool_free_unlocked(block);
    taskEXIT_CRITICAL();
}

void pwos_frame_pool_free_from_isr(pwos_frame_block_t *block)
{
    UBaseType_t saved_mask;

    saved_mask = taskENTER_CRITICAL_FROM_ISR();
    frame_pool_free_unlocked(block);
    taskEXIT_CRITICAL_FROM_ISR(saved_mask);
}

size_t pwos_frame_pool_free_count(void)
{
    size_t count;

    taskENTER_CRITICAL();
    count = g_free_count;
    taskEXIT_CRITICAL();

    return count;
}

size_t pwos_frame_pool_capacity(void)
{
    return PWOS_FRAME_POOL_CAPACITY;
}

uint32_t pwos_frame_pool_alloc_fail_count(void)
{
    uint32_t count;

    taskENTER_CRITICAL();
    count = g_alloc_fail_count;
    taskEXIT_CRITICAL();

    return count;
}
