#ifndef MCC_PASER_H
#define MCC_PASER_H

#include <stddef.h>

#include "stb_c_lexer.h"

typedef enum {
  ARG_NAME,     // 暂时只能以名称的引用的参数，作为编译时的占位符
  ARG_LIT_INT,
} ArgKind;

typedef struct {
  ArgKind kind;
  union {
    char *name;
    int   num_int;
  };
} Arg;

typedef struct {
  Arg *items;
  size_t count;
  size_t capacity;
} ArgList;

typedef enum {
  OP_INVOKE,
  OP_RETURN,
} OpKind;

typedef struct {
  OpKind kind;
  union {
    struct {
      Arg fn;
      ArgList args;
    };
    struct {
      Arg ret_val;
    };
  };
} Op;

typedef struct {
  Op *items;
  size_t count;
  size_t capacity;
} OpList;

typedef struct {
  bool external;
  char *name;
  OpList fn_body;
} Symbol;

typedef struct {
  Symbol *items;
  size_t count;
  size_t capacity;
} SymbolTable;

bool compile_file(stb_lexer *l, const char *filename, SymbolTable *syms);
void destroy_symtable(SymbolTable *syms);

#endif // MCC_PASER_H
