// reference: https://github.com/tsoding/arena/blob/master/arena.h
// this is a wrapper and replacement of realloc/malloc
// thus, it is slitly different from tsoding's implementation.

#ifndef ARENA_H
#define ARENA_H

#include <stdlib.h>

typedef struct {
  size_t size;
  char data[];
} Arena_Slot;

typedef struct Arena_Block Arena_Block;
struct Arena_Block {
  Arena_Block *next;
  size_t capacity;
  char data[];
};

Arena_Block *new_arena_block(size_t capacity);
void free_arena_block(Arena_Block *r);

typedef struct {
  Arena_Block *begin;
  Arena_Block *end;
  size_t used;
} Arena;

void   set_arena(Arena *arena);
Arena *get_arena();

extern Arena default_arena;

void *arena_alloc(size_t size);
void *arena_calloc(size_t n, size_t size);
void *arena_realloc(void *ptr, size_t new_size);

typedef struct  {
  Arena_Block *end;
  size_t used;
} Arena_Mark;

Arena_Mark arena_snapshot(Arena *a);
void arena_reset(Arena *a);
void arena_rewind(Arena *a, Arena_Mark m);
void arena_free(Arena *a);
void arena_trim(Arena *a);

#endif // ARENA_H

#ifdef ARENA_IMPLEMENTATION

#ifndef ARENA_DEFAULT_CAPACITY
#define ARENA_DEFAULT_CAPACITY (8 << 10)
#endif // ARENA_DEFAULT_CAPACITY

Arena  default_arena = {0};
Arena *current_arena = &default_arena;

void set_arena(Arena *arena)
{
  assert(arena);
  current_arena = arena;
}

Arena *get_arena()
{
  return current_arena;
}

Arena_Block *new_arena_block(size_t capacity)
{
  Arena_Block *block = malloc(sizeof(Arena_Block) + capacity);
  block->next     = NULL;
  block->capacity = capacity;

  return block;
}

void free_arena_block(Arena_Block *r)
{
  free(r);
}

void *arena_alloc(size_t size)
{
  size_t alloc_size = size + sizeof(Arena_Slot);

  if (current_arena->end == NULL) {
    assert(current_arena->begin == NULL);

    size_t capacity = ARENA_DEFAULT_CAPACITY;
    if (capacity < size) capacity = size;

    current_arena->end   = new_arena_block(capacity);
    current_arena->begin = current_arena->end;
    current_arena->used  = 0;
  }

  while (current_arena->end->next != NULL) {
    if (current_arena->used + alloc_size < current_arena->end->capacity)
      break;

    current_arena->end  = current_arena->end->next;
    current_arena->used = 0;
  }

  if (current_arena->used + alloc_size > current_arena->end->capacity) {
    assert(current_arena->end->next == NULL);

    size_t capacity = ARENA_DEFAULT_CAPACITY;
    if (capacity < size) capacity = size;

    current_arena->end->next = new_arena_block(capacity);
    current_arena->end       = current_arena->end->next;
    current_arena->used      = 0;
  }

  Arena_Slot *slot = (Arena_Slot*)&current_arena->end->data[current_arena->used];
  current_arena->used += alloc_size;
  slot->size = size;
  return slot->data;
}

void *arena_calloc(size_t n, size_t size)
{
  void *mem = arena_alloc(n * size);
  return memset(mem, 0, n * size);
}

void *arena_realloc(void *ptr, size_t new_size)
{
  void *new_ptr = arena_alloc(new_size);
  if (ptr != NULL) {
    Arena_Slot *slot = ptr - sizeof(Arena_Slot);
    memcpy(new_ptr, ptr, slot->size);
  }
  return new_ptr;
}

Arena_Mark arena_snapshot(Arena *a)
{
  assert(a);
  return (Arena_Mark) {
    .end  = a->end,
    .used = a->used,
  };
}

void arena_reset(Arena *a)
{
  assert(a);
  a->end = a->begin;
  a->used = 0;
}

void arena_rewind(Arena *a, Arena_Mark m)
{
  a->end  = m.end;
  a->used = m.used;
}

void arena_free(Arena *a)
{
  Arena_Block *b = a->begin;
  while (b) {
    Arena_Block *b0 = b;
    b = b->next;
    free_arena_block(b0);
  }
  a->begin = NULL;
  a->end   = NULL;
}

void arena_trim(Arena *a)
{
  Arena_Block *b = a->end->next;
  while (b) {
    Arena_Block *b0 = b;
    b = b->next;
    free_arena_block(b0);
  }
  a->end->next = NULL;
}

#endif // ARENA_IMPLEMENTATION

