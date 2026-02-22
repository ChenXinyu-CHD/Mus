#ifndef MCC_PASER_H
#define MCC_PASER_H

#include <stddef.h>

#include "nob.h"
#include "stb_c_lexer.h"

typedef enum {
  TYPE_VOID,
  TYPE_INT,
  TYPE_UINT,
} TypeKind;

typedef struct {
  TypeKind kind;
  size_t size;
} TypeExpr;

typedef enum {
  ARG_NONE,
  ARG_NAME,     // 暂时只能以名称的引用的参数，作为编译时的占位符
  ARG_VAR_LOC,
  ARG_LIT_INT,
  ARG_LIT_STR,
} ArgKind;

typedef struct {
  ArgKind kind;
  union {
    char   *name;
    int    num_int;
    size_t label;
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
  OP_SET_VAR,
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
    struct {
      Arg var;
      Arg val;
    };
  };
} Op;

typedef struct {
  Op *items;
  size_t count;
  size_t capacity;
} OpList;

typedef struct {
  char *name;
} Extern;

typedef struct {
  Extern *items;
  size_t count;
  size_t capacity;
} ExternList;

typedef struct {
  char *name;
} Var;

typedef struct {
  Var *items;
  size_t count;
  size_t capacity;
} VarList;

typedef struct {
  char *name;
  TypeExpr ret_type;
  OpList fn_body;
  VarList local;
} Fn;

typedef struct {
  Fn *items;
  size_t count;
  size_t capacity;
} FnList;

typedef struct {
  FnList fn_list;
  ExternList externs;
  struct {
    char **items;
    size_t count;
    size_t capacity;
  } str_lits;
} Program;

bool compile_file(stb_lexer *l, const char *filename, Program *grog);
void destroy_program(Program *prog);

#endif // MCC_PASER_H
