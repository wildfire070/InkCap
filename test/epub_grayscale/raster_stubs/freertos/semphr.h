#pragma once
#include "FreeRTOS.h"
#include "task.h"
inline void* xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline void* xSemaphoreCreateRecursiveMutex() { return reinterpret_cast<void*>(1); }
inline int xSemaphoreTake(void*, unsigned) { return pdTRUE; }
inline void xSemaphoreGive(void*) {}
inline int xSemaphoreTakeRecursive(void*, unsigned) { return pdTRUE; }
inline void xSemaphoreGiveRecursive(void*) {}
inline void* xSemaphoreGetMutexHolder(void*) { return reinterpret_cast<void*>(1); }
