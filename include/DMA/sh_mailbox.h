#pragma once
#include <stdint.h>

typedef struct
{
  union
  {
    uint32_t allocHandle;
    uint32_t bAddr;
  };

  void *vAddr;
  uintptr_t pAddr;
  uint32_t len;
} UncachedMem;

uint32_t sh_init();
uint32_t sh_fini();

uint32_t sh_alloc(const uint32_t len, UncachedMem *mem);
void sh_dealloc(const UncachedMem *mem);