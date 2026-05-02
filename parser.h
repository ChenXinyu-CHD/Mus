#ifndef MCC_PASER_H
#define MCC_PASER_H

#include <stddef.h>

#include "nob.h"
#include "lexer.h"

typedef enum {
  TYPE_UNKNOWN,
  TYPE_VOID,
  TYPE_INT,
  TYPE_UINT,
  TYPE_FN,
  TYPE_PTR,
} TypeKind;

#define TYPE_KIND_COUNT 6

typedef struct TypeExpr TypeExpr;

typedef struct {
  TypeExpr *items;
  size_t count;
  size_t capacity;
} TypeList;

typedef struct {
  TypeExpr *ret_type;
  TypeList arg_types;
  bool va_args;
} FnType;

struct TypeExpr {
  TypeKind kind;
  size_t size;

  union {
    TypeExpr *ref_type;
    FnType fn_type;
  };
};

void dump_type_expr(TypeExpr *type, FILE *stream);

typedef enum {
  ARG_NONE,
  ARG_NAME,     // 暂时只能以名称的引用的参数，作为编译时的占位符
  ARG_EXTERN,
  ARG_FN,
  ARG_VAR_LOC,
  ARG_VAR_ARG,
  ARG_LIT_INT,
  ARG_LIT_STR,
} ArgKind;

typedef struct {
  ArgKind kind;
  TypeExpr type;
  union {
    struct {
      String_View name;
      Cursor loc;
    };
    int    num_int;
    size_t label;
  };
} Arg;

#define label_item(table, label) (table)->items[(assert(label < (table)->count), (label))]

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
  Arg fn;
  ArgList args;
  bool ret_ignore;
  size_t result_label;
} OpInvoke;

typedef struct {
  Arg var;
  Arg val;
} OpSetVar;

typedef struct {
  OpKind kind;
  Cursor loc;
  union {
    OpInvoke invoke;
    Arg ret_val;
    OpSetVar set_var;
  };
} Op;

typedef struct {
  Op *items;
  size_t count;
  size_t capacity;
} OpList;

typedef struct {
  String_View name;
  Cursor loc;
  TypeExpr type;
} Extern;

typedef struct {
  Extern *items;
  size_t count;
  size_t capacity;
} ExternList;

typedef struct {
  String_View name;
  Cursor loc;
  TypeExpr type;
  
  ptrdiff_t offset;
} Var;

typedef struct {
  Var *items;
  size_t memsize;
  
  size_t count;
  size_t capacity;
} VarList;

size_t alloc_var(VarList *vars);

typedef struct {
  String_View name;
  Cursor loc;
  TypeExpr type;
  
  OpList fn_body;
  VarList local;
  VarList args;
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
    String_View *items;
    size_t count;
    size_t capacity;
  } str_lits;
} Program;

bool compile_program(Lexer *l, Program *grog);
void destroy_program(Program *prog);

#endif // MCC_PASER_H
