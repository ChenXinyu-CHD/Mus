#ifndef MCC_UTILS_H
#define MCC_UTILS_H

#include "nob.h"

size_t ceil_to(size_t size, size_t align);

struct mcc_utils__dict {
  void *items;
  String_View *name_ptr;
  size_t item_size;
  size_t count;
};

#define mcc_utils__to_dict(table) ((struct mcc_utils__dict) {   \
      .items = table->items,                                    \
      .name_ptr = &table->items->name,                          \
      .item_size = sizeof(*table->items),                       \
      .count = table->count,                                    \
    })

void  *mcc_utils__dct_getv_impl(struct mcc_utils__dict dict, String_View name);
size_t mcc_utils__dct_geti_impl(struct mcc_utils__dict dict, String_View name);

#define dct_getv(st, name) mcc_utils__dct_getv_impl(mcc_utils__to_dict((st)), (name))
#define dct_geti(st, name) mcc_utils__dct_geti_impl(mcc_utils__to_dict((st)), (name))
#define dct_contains(st, name) (mcc_utils__dct_getv_impl(mcc_utils__to_dict((st)), (name)) != NULL)

#endif // MCC_UTILS_H

#ifdef MCC_UTILS_IMPLEMENTATION

size_t ceil_to(size_t size, size_t align)
{
  size_t mod = size % align;
  if (mod == 0) return size;

  return size - mod + align;
}

void  *mcc_utils__dct_getv_impl(struct mcc_utils__dict dict, String_View name)
{
  size_t i = mcc_utils__dct_geti_impl(dict, name);
  if (i >= dict.count) return NULL;
  
  return dict.items + (i * dict.item_size);
}

size_t mcc_utils__dct_geti_impl(struct mcc_utils__dict dict, String_View name)
{
  size_t i = 0;
  String_View *name_ptr = dict.name_ptr;
  while (i < dict.count) {
    if (sv_eq(*name_ptr, name)) {
      break;
    }
    i += 1;
    name_ptr = (String_View *)((char *)(name_ptr) + dict.item_size);
  }

  return i;
}

#endif
