#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

// FreeRTOS xTaskCreate* always takes stacks from internal DRAM (pvPortMalloc).
// USB Host / audio tasks need 4–8 KB each; after camera and Wi-Fi that DRAM is
// fragmented. Put the stack in PSRAM and keep the TCB in internal RAM.
static inline BaseType_t xiaopai_task_create_psram(TaskFunction_t fn,
                                                   const char* name,
                                                   uint32_t stack_bytes,
                                                   void* arg,
                                                   UBaseType_t prio,
                                                   TaskHandle_t* out,
                                                   BaseType_t core)
{
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        fn, name, stack_bytes, arg, prio, out, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW("xiaopai_task", "Create %s in SPIRAM failed, fallback to internal DRAM", name);
        ok = xTaskCreatePinnedToCoreWithCaps(
            fn, name, stack_bytes, arg, prio, out, core, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ok;
}

static inline BaseType_t xiaopai_task_create_psram_unpinned(TaskFunction_t fn,
                                                            const char* name,
                                                            uint32_t stack_bytes,
                                                            void* arg,
                                                            UBaseType_t prio,
                                                            TaskHandle_t* out)
{
    return xiaopai_task_create_psram(fn, name, stack_bytes, arg, prio, out, tskNO_AFFINITY);
}
