#include "schedule.h"

#include <time.h>

uint32_t rtos_now_time(void)
{
    return (uint32_t)((clock() * 1000u) / CLOCKS_PER_SEC);
}
