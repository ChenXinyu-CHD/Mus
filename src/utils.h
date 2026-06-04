#ifndef MCC_UTILS_H
#define MCC_UTILS_H

#include "nob.h"
#include "ht.h"

size_t ceil_to(size_t size, size_t align);

#endif // MCC_UTILS_H

#ifdef MCC_UTILS_IMPLEMENTATION

size_t ceil_to(size_t size, size_t align)
{
  size_t mod = size % align;
  if (mod == 0) return size;

  return size - mod + align;
}

#endif
