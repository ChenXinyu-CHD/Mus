#ifndef MCC_PASER_H
#define MCC_PASER_H

#include <stddef.h>

#include "nob.h"
#include "stb_c_lexer.h"

typedef enum {
  TYPE_UNKNOWN,
  TYPE_VOID,
  TYPE_INT,
  TYPE_UINT,
  TYPE_FN,
  TYPE_PTR,
} TypeKind;

struct TypeExpr;

typedef struct {
  struct TypeExpr *items;
  size_t count;
  size_t capacity;
} TypeList;

typedef struct TypeExpr {
  TypeKind kind;
  size_t size;

  union {
    struct TypeExpr *ref_type;
    struct {
      struct TypeExpr *ret_type;
      TypeList arg_types;
      bool va_args;
    };
  };
} TypeExpr;

typedef enum {
  ARG_NONE,
  ARG_NAME,     // 暂时只能以名称的引用的参数，作为编译时的占位符
  ARG_GLOBAL,
  ARG_VAR_LOC,
  ARG_LIT_INT,
  ARG_LIT_STR,
} ArgKind;

typedef struct {
  ArgKind kind;
  TypeExpr type;
  union {
    struct {
      char *name;
      stb_lex_location loc;
    };
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
  bool external;
  char *name;
  TypeExpr type;
} Symbol;

typedef struct {
  Symbol *items;
  size_t count;
  size_t capacity;
} SymbolList;

typedef struct {
  char *name;
  OpList fn_body;
  SymbolList local;
} Fn;

typedef struct {
  Fn *items;
  size_t count;
  size_t capacity;
} FnList;

typedef struct {
  const char *filename;
  FnList fn_list;
  SymbolList global;
  struct {
    char **items;
    size_t count;
    size_t capacity;
  } str_lits;
} Program;

bool compile_file(stb_lexer *l, const char *filename, Program *grog);
void destroy_program(Program *prog);

#endif // MCC_PASER_H
