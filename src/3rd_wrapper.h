#ifndef MCC_3RD_WRAPPER_H
#define MCC_3RD_WRAPPER_H

#include "arena.h"

#define NOB_REALLOC arena_realloc
#define NOB_FREE (void)
#include "nob.h"

#define HT_FREE (void)
#define HT_MALLOC arena_alloc
#include "ht.h"

#endif // MCC_3RD_WRAPPER_H

#ifdef MCC_3RD_WRAPPER_IMPLEMENTATION

#define ARENA_IMPLEMENTATION
#define NOB_IMPLEMENTATION
#define HT_IMPLEMENTATION

#include "arena.h"
#include "nob.h"
#include "ht.h"

#endif // MCC_3RD_WRAPPER_IMPLEMENTATION

