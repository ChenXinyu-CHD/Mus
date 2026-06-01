#ifndef MCC_PASER_H
#define MCC_PASER_H

#include <stddef.h>

#include "3rd/nob.h"
#include "3rd/ht.h"

#include "lexer.h"
#include "ast.h"
#include "SymbolTable.h"
#include "type.h"

typedef enum {
  ARG_NONE = 0,
  ARG_EXTERN,
  ARG_FN,
  ARG_VAR,
  ARG_LIT_INT,
  ARG_LIT_STR,
  __arg_kind_count,
} ArgKind;

typedef struct Fn Fn;

typedef struct {
  ArgKind kind;
  TypeExpr type;
  union {
    struct {
      String_View name;
      Cursor loc;
      Scoop *scoop;
    };
    int num_int;
    Fn *fn;
    Var *var;
    Extern *ext;
    size_t str_label;
  };
} Arg;

typedef struct {
  Arg *items;
  size_t count;
  size_t capacity;
} ArgList;

typedef enum {
  OP_INVOKE = 0,
  OP_RETURN,
  OP_SET_VAR,
  OP_BINOP,
  OP_JMP,
  OP_JMP_ELSE,
  OP_LABEL,
  __op_kind_count,
} OpKind;

typedef struct {
  Arg fn;
  ArgList args;
  bool ret_ignore;
  Arg ret;
} OpInvoke;

typedef struct {
  Arg var;
  Arg val;
} OpSetVar;

typedef struct {
  BinopKind kind;
  Arg lhs;
  Arg rhs;
  Arg dst;
} OpBinop;

typedef struct {
  size_t label;
  Arg cond;
} OpJmp;

const char *binop_name(BinopKind kind);

typedef struct {
  OpKind kind;
  Cursor loc ;
  union {
    OpInvoke invoke;
    Arg ret_val;
    OpSetVar set_var;
    OpBinop binop;
    OpJmp jmp;
    size_t label;
  };
} Op;

void dump_op(String_Builder *sb, Op *op);

typedef struct {
  Op *items;
  size_t count;
  size_t capacity;
} OpList;

struct Extern {
  String_View linkname;
  Cursor loc;
  TypeExpr type;
};

Var *alloc_var(VarList *vars);

struct Fn {
  String_Builder name;
  TypeExpr type;
  Cursor loc;

  OpList fn_body;
  VarList vars;
};

typedef struct {
  Fn **items;
  size_t count;
  size_t capacity;
} FnList;

struct Program {
  FnList fn_list;
  ExternList externs;
  struct {
    String_View *items;
    size_t count;
    size_t capacity;
  } str_lits;
};

bool compile_program(Lexer *l, Program *grog);
void destroy_program(Program *prog);

#endif // MCC_PASER_H
