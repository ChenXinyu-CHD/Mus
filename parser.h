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
  ARG_NAME,     // 暂时只能以名称的引用的参数，作为编译时的占位符
  ARG_EXTERN,
  ARG_FN,
  ARG_VAR_LOC,
  ARG_VAR_ARG,
  ARG_LIT_INT,
  ARG_LIT_STR,
  __arg_kind_count,
} ArgKind;

typedef struct {
  ArgKind kind;
  TypeExpr type;
  union {
    struct {
      String_View name;
      Cursor loc;
      Scoop *scoop;
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
  size_t result_label;
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
  };
} Op;

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

size_t alloc_var(VarList *vars);

struct Fn {
  TypeExpr type;
  Cursor loc;
  
  OpList fn_body;
  VarList vars;
  VarList args;
  Scoop *local;
};

typedef struct {
  Fn **items;
  size_t count;
  size_t capacity;
} FnList;

typedef struct {
  Scoop **items;
  size_t count;
  size_t capacity;
} SymbolTable;

struct Program {
  SymbolTable symbols;
  Scoop *global;
  
  FnList fn_list;
  ExternList externs;
  struct {
    String_View *items;
    size_t count;
    size_t capacity;
  } str_lits;
};

Scoop *alloc_scoop(SymbolTable *st, Scoop *upper);
void free_all_symbol(SymbolTable *st);

bool compile_program(Lexer *l, Program *grog);
void destroy_program(Program *prog);

#endif // MCC_PASER_H
