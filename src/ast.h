#ifndef MCC_AST_H_
#define MCC_AST_H_

#include "nob.h"

#include "type.h"

typedef enum {
  EXPR_ATOM = 0,
  EXPR_INVOKE,
  EXPR_BINOP,
  __expr_kind_count
} Expr_Kind;

typedef struct Expr Expr;

typedef struct {
  Expr *items;
  size_t count;
  size_t capacity;
} Expr_List;

typedef enum {
  BINOP_ADD = 0,
  BINOP_SUB,
  BINOP_MUL,
  BINOP_DIV,
  BINOP_MOD,
  BINOP_EQ,
  BINOP_NEQ,
  BINOP_LS,
  BINOP_GT,
  BINOP_LE,
  BINOP_GE,
  __binop_kind_count,
} BinopKind;

typedef struct {
  Expr* fn;
  Expr_List args;
} AST_Invoke;

struct Expr {
  Expr_Kind kind;
  Cursor loc;

  union {
    Token atom;
    AST_Invoke invoke;
    struct {
      BinopKind kind;
      Expr *lhs;
      Expr *rhs;
    } binop;
  };
};

bool compile_expr(Lexer *l, Expr *expr);
void expr_del(Expr *expr);

typedef enum {
  STAT_EMPTY = 0,
  STAT_INVOKE,
  STAT_RET,
  STAT_ASSIGN,
  STAT_BLOCK,
  STAT_IF,
  STAT_DEF,
  __stat_kind_count,
} Stat_Kind;

typedef struct Stat Stat;

typedef struct {
  Stat *items;
  size_t count;
  size_t capacity;
} Stat_List;

typedef enum {
  DEF_VAR,
  DEF_FN,
  DEF_EXT,
  __def_kind_count,
} Def_Kind;

typedef struct {
  String_View linkname;
  TypeExpr type;
} Extern;

typedef struct Def Def;

typedef struct {
  Cursor loc;

  TypeExpr ret_type;
  struct {
    Def *items;
    size_t count;
    size_t capacity;
  } args;

  Stat_List body;
} AST_Fn;

struct Def {
  Def_Kind kind;
  String_View name;
  Cursor loc;
  union {
    struct {
      TypeExpr type;
      Expr *init;
    } var;
    Extern ext;
    AST_Fn fn;
  };
};

struct Stat {
  Stat_Kind kind;
  Cursor loc;
  union {
    AST_Invoke invoke;
    Expr *ret_val;
    Def def;
    struct {
      Expr *dst;
      Expr *val;
    } assign;
    Stat_List block;
    struct {
      Expr *cond;
      Stat *on_true;
      Stat *on_false;
    } if_else;
  };
};

bool compile_stat(Lexer *l, Stat *stat);
bool compile_block(Lexer *l, Stat_List *block);
void stat_del(Stat *stat);

bool compile_file(Lexer *l, Stat_List *stats);
void stat_list_del(Stat_List stats);

#endif // MCC_AST_H_

#ifdef MCC_AST_IMPLEMENTATION

static const struct {
  int token_kind;
  BinopKind binop_kind;
} binop_list[] = {
  {.token_kind =  '+',       .binop_kind = BINOP_ADD,},
  {.token_kind =  '-',       .binop_kind = BINOP_SUB,},
  {.token_kind =  '*',       .binop_kind = BINOP_MUL,},
  {.token_kind =  '/',       .binop_kind = BINOP_DIV,},
  {.token_kind =  '%',       .binop_kind = BINOP_MOD,},
  {.token_kind =  '<',       .binop_kind = BINOP_LS,},
  {.token_kind =  '>',       .binop_kind = BINOP_GT,},
  {.token_kind =  TOKEN_EQ,  .binop_kind = BINOP_EQ,},
  {.token_kind =  TOKEN_NEQ, .binop_kind = BINOP_NEQ,},
  {.token_kind =  TOKEN_LE,  .binop_kind = BINOP_LE,},
  {.token_kind =  TOKEN_GE,  .binop_kind = BINOP_GE,},
};
static_assert(__binop_kind_count == ARRAY_LEN(binop_list),
              "introduced more binop kinds");

const char *binop_name(BinopKind kind)
{
  for (size_t i = 0; i < ARRAY_LEN(binop_list); ++i) {
    if (binop_list[i].binop_kind == kind) {
      return token_name(binop_list[i].token_kind);
    }
  }
  UNREACHABLE("");
}

static Expr expr_atom(Token token)
{
  return (Expr) {
    .kind = EXPR_ATOM,
    .loc = token.start,
    .atom = token
  };
}
static Expr expr_invoke(Expr *fn, Expr_List args)
{
  return (Expr) {
    .kind = EXPR_INVOKE,
    .loc = fn->loc,
    .invoke =  {
      .fn = fn,
      .args = args,
    }
  };
}
static Expr expr_binop(Token op, Expr *lhs, Expr *rhs)
{
  for (size_t i = 0; i < ARRAY_LEN(binop_list); ++i) {
    if (binop_list[i].token_kind == op.kind) {
      return (Expr) {
        .kind = EXPR_BINOP,
        .loc = op.start,
        .binop = {
          .kind = binop_list[i].binop_kind,
          .lhs = lhs,
          .rhs = rhs,
        }
      };
    }
  }
  UNREACHABLE("");
}

static void expr_list_del(Expr_List *exprs)
{
  da_foreach (Expr, expr, exprs) {
    expr_del(expr);
  }
  da_free(*exprs);
}

void expr_del(Expr *expr)
{
  static_assert(__expr_kind_count == 3, "introduced more expr kinds");
  switch(expr->kind) {
  case EXPR_ATOM:
    return;
  case EXPR_INVOKE:
    expr_del(expr->invoke.fn);
    expr_list_del(&expr->invoke.args);
    return;
  case EXPR_BINOP:
    expr_del(expr->binop.lhs);
    expr_del(expr->binop.rhs);
    return;
  default: UNREACHABLE("");
  }
}

static bool compile_invoke_args(Lexer *l, Expr_List *args)
{
  assert(l->current.kind == '(');
  bool result;
  if (!prefetch_not_none(l)) return_defer(false);

  while (l->current.kind != ')') {
    Expr arg;
    if (!compile_expr(l, &arg)) return_defer(false);
    da_append(args, arg);

    if (!expect_tokens(l, ',', ')')) return_defer(false);

    if (l->current.kind == ',') {
      if (!prefetch_not_none(l)) return_defer(false);
    }
  }
  if (!prefetch_not_none(l)) return_defer(false);

  return true;
 defer:
  expr_list_del(args);
  return result;
}

static bool compile_simple_expr(Lexer *l, Expr *expr)
{
  bool result;
  if (!expect_tokens(l,
                     TOKEN_STR,
                     TOKEN_INT,
                     TOKEN_ID,
                     TOKEN_TRUE,
                     TOKEN_FALSE,
                     '('))
    return false;

  if (l->current.kind == '(') {
    if (!prefetch_not_none(l))  return_defer(false);
    if (!compile_expr(l, expr)) return_defer(false);
    if (!expect_token(l, ')'))  return_defer(false);
  } else {
    *expr = expr_atom(l->current);
  }

  if (!prefetch_not_none(l)) return_defer(false);
  while (l->current.kind == '(') {
    Expr_List args = {0};
    if (!compile_invoke_args(l, &args)) return_defer(false);

    Expr *fn = malloc(sizeof(Expr));
    *fn = *expr;
    *expr = expr_invoke(fn, args);
  }

  return true;
 defer:
  expr_del(expr);
  return result;
}

static bool compile_mul(Lexer *l, Expr *expr)
{
  if (!compile_simple_expr(l, expr)) return false;

  bool result;
  while (l->current.kind == '*' || l->current.kind == '/' || l->current.kind == '%') {
    Token op = l->current;
    if (!prefetch_not_none(l)) return_defer(false);

    Expr rhs_expr = {0};
    if (!compile_simple_expr(l, &rhs_expr)) return_defer(false);

    Expr *lhs = malloc(sizeof(*lhs));
    *lhs = *expr;
    Expr *rhs = malloc(sizeof(*rhs));
    *rhs = rhs_expr;

    *expr = expr_binop(op, lhs, rhs);
  }

  return true;
 defer:
  expr_del(expr);
  return result;
}

static bool compile_add(Lexer *l, Expr *expr)
{
  if (!compile_mul(l, expr)) return false;

  bool result;
  while (l->current.kind == '+' || l->current.kind == '-') {
    Token op = l->current;
    if (!prefetch_not_none(l)) return_defer(false);

    Expr rhs_expr = {0};
    if (!compile_mul(l, &rhs_expr)) return_defer(false);

    Expr *lhs = malloc(sizeof(*lhs));
    *lhs = *expr;
    Expr *rhs = malloc(sizeof(*rhs));
    *rhs = rhs_expr;

    *expr = expr_binop(op, lhs, rhs);
  }

  return true;
 defer:
  expr_del(expr);
  return result;
}

static bool compile_cmp(Lexer *l, Expr *expr)
{
  if (!compile_add(l, expr)) return false;

  bool result;
  while (l->current.kind == TOKEN_EQ ||
         l->current.kind == TOKEN_LE ||
         l->current.kind == TOKEN_GE ||
         l->current.kind == '<' ||
         l->current.kind == '>' ||
         l->current.kind == TOKEN_NEQ) {
    Token op = l->current;
    if (!prefetch_not_none(l)) return_defer(false);

    Expr rhs_expr = {0};
    if (!compile_add(l, &rhs_expr)) return_defer(false);

    Expr *lhs = malloc(sizeof(*lhs));
    *lhs = *expr;
    Expr *rhs = malloc(sizeof(*rhs));
    *rhs = rhs_expr;

    *expr = expr_binop(op, lhs, rhs);
  }

  return true;
 defer:
  expr_del(expr);
  return result;
}

bool compile_expr(Lexer *l, Expr *expr)
{
  // EXPR   :: CMP
  // CMP    :: ADD | ADD == CMP | ADD != CMP | ADD < CMP | ADD > CMP | ADD <= CMP | ADD >= CMP
  // ADD    :: MUL | MUL + ADD | MUL - ADD
  // MUL    :: SIMPLE | SIMPLE * MUL | SIMPLE / MUL | SIMPLE % MUL
  // SIMPLE :: ATOM | INVOKE | ( EXPR )
  // ATOM   :: STR | INT | ID
  // INVOKE :: EXPR ( ARGS )
  // ARGS   :: EXPR | EXPR , ARGS
  return compile_cmp(l, expr);
}

bool compile_type_expr(Lexer *l, TypeExpr *type);

static bool compile_var(Lexer *l, Def *def, bool prefix)
{
  if (prefix) {
    assert(l->current.kind == TOKEN_VAR);
    if (!prefetch_not_none(l)) return false;
  }
  if (!expect_token(l, TOKEN_ID)) return false;

  *def = (Def) {
    .kind = DEF_VAR,
    .name = l->current.str,
    .var = {
      .type = {.kind=TYPE_UNKNOWN},
      .init = NULL,
    },
  };

  if (!prefetch_not_none(l)) return false;

  if (l->current.kind == ':') {
    if (!prefetch_not_none(l)) return false;
    if (!compile_type_expr(l, &def->var.type)) return false;
    if (def->var.type.kind == TYPE_VOID) {
      pcompile_info(l->current.start,
                    "error: the type of a local variable cannot be \"void\"");
      return false;
    }
    if (!prefetch_not_none(l)) return false;
  }

  if (l->current.kind == '=') {
    if (!prefetch_not_none(l)) return false;

    Expr *val = calloc(1, sizeof(*val));
    if (!compile_expr(l, val)) {
      free(val);
      return false;
    }
    def->var.init = val;
  }

  return true;
}

bool compile_fn(Lexer *l, Def *def)
{
  assert(l->current.kind == TOKEN_FN);

  if (!prefetch_expect_token(l, TOKEN_ID)) return false;
  *def = (Def) {
    .kind = DEF_FN,
    .name = l->current.str,
    .fn = {
      .loc = l->current.start,
    },
  };

  if (!prefetch_expect_token(l, '(')) return false;
  if (!prefetch_expect_tokens(l, ')', TOKEN_ID)) return false;

  while (l->current.kind != ')') {
    assert(l->current.kind == TOKEN_ID);

    Def arg_def = {0};
    if (!compile_var(l, &arg_def, false)) return false;
    if (!expect_tokens(l, ',', ')')) return false;

    if (l->current.kind == ',') {
      if (!prefetch_expect_token(l, TOKEN_ID)) return false;
    }
    da_append(&def->fn.args, arg_def);
  }
  assert(l->current.kind == ')');

  if (!prefetch_expect_token(l, ':')) return false;
  if (!prefetch_not_none(l)) return false;

  if (!compile_type_expr(l, &def->fn.ret_type)) return false;

  if (!prefetch_expect_token(l, '{')) return false;
  return compile_block(l, &def->fn.body);
}

bool compile_stat(Lexer *l, Stat *stat)
{
  *stat = (Stat) {
    .kind = STAT_EMPTY,
    .loc = l->current.start,
  };

  if (l->current.kind == '{') {
    if (l->current.kind == '{') {
      stat->kind = STAT_BLOCK;
      return compile_block(l, &stat->block);
    }
  }

  bool result = false;
  // simple statement
  if (l->current.kind == TOKEN_EXT) {
    stat->kind = STAT_DEF;
    if (!prefetch_expect_token(l, TOKEN_ID)) return false;
    stat->def = (Def) {
      .kind = DEF_EXT,
      .loc = l->current.start,
      .name = l->current.str,
      .ext = {
        .linkname = l->current.str,
      },
    };

    if (!prefetch_expect_token(l, ':')) return false;
    if (!prefetch_not_none(l)) return false;
    if (!compile_type_expr(l, &stat->def.ext.type)) return false;

    lexer_next(l);
  } else if (l->current.kind == TOKEN_FN) {
    stat->kind = STAT_DEF;
    if (!compile_fn(l, &stat->def)) return_defer(false);
  } else if (l->current.kind == TOKEN_VAR) {
    stat->kind = STAT_DEF;
    if (!compile_var(l, &stat->def, true)) return_defer(false);
  } else if (l->current.kind == TOKEN_IF) {
    stat->kind = STAT_IF;
    if (!prefetch_not_none(l)) return_defer(false);

    stat->if_else.cond = calloc(1, sizeof(Expr));
    if (!compile_expr(l, stat->if_else.cond)) return_defer(false);

    stat->if_else.on_true = calloc(1, sizeof(Stat));
    if (!compile_stat(l, stat->if_else.on_true)) return_defer(false);

    stat->if_else.on_false = NULL;
    if (l->current.kind == TOKEN_ELSE) {
      if (!prefetch_not_none(l)) return_defer(false);
      stat->if_else.on_false = calloc(1, sizeof(Stat));
      if (!compile_stat(l, stat->if_else.on_false)) return_defer(false);
    }
  } else if (l->current.kind == TOKEN_RET) {
    stat->kind = STAT_RET;

    if (!prefetch_not_none(l)) return_defer(false);

    stat->ret_val = calloc(1, sizeof(Expr));

    if (!compile_expr(l, stat->ret_val)) return_defer(false);
  } else {
    Expr expr = {0};
    if (!compile_expr(l, &expr)) return_defer(false);

    if (l->current.kind == '=') {
      stat->kind = STAT_ASSIGN;
      stat->assign.dst = calloc(1, sizeof(Expr));
      *stat->assign.dst = expr;

      if (!prefetch_not_none(l)) return_defer(false);
      stat->assign.val = calloc(1, sizeof(Expr));
      if (!compile_expr(l, stat->assign.val)) return_defer(false);
    } else if (expr.kind == EXPR_INVOKE) {
      stat->kind = STAT_INVOKE;
      stat->invoke = expr.invoke;
    } else {
      pcompile_info(stat->loc,
                    "error: expect a statement, but got an expression.\n");
      expr_del(&expr);
      return_defer(false);
    }
  }

  // a simple statment can be followed with an optional ';'
  // This makes "if true foo(); else bar();" acceptable
  // because 'foo();' is a single statement
  // insteed of a function call followed by a empty statement.
  // "if true ;; else foo()" is not acceptable
  // because both ';' are two empty statement
  // because they are not followed by a simple statement
  if (l->current.kind == ';') {
    lexer_next(l);
  }

  return true;
 defer:
  if (!result) stat_del(stat);
  return result;
}

bool compile_block(Lexer *l, Stat_List *block)
{
  assert(l->current.kind == '{');
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != '}') {
    Stat s = {0};
    if (!compile_stat(l, &s)) return false;
    da_append(block, s);
  }
  assert(l->current.kind == '}');
  lexer_next(l);
  return true;
}

bool compile_file(Lexer *l, Stat_List *stats)
{
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != TOKEN_EOF && l->current.kind != TOKEN_ERR) {
    Stat s = {0};
    if (!compile_stat(l, &s)) return false;
    da_append(stats, s);
  }
  return l->current.kind == TOKEN_EOF;
}

void stat_list_del(Stat_List stats)
{
  da_foreach(Stat, stat, &stats) {
    stat_del(stat);
  }
  da_free(stats);
}

static void del_fn_ast(AST_Fn *fn);

static void del_def(Def *def)
{
  static_assert(__def_kind_count == 3,
                "introduced more def kinds");
  switch (def->kind) {
  case DEF_VAR:
    destroy_type_expr(&def->var.type);
    if (def->var.init != NULL) {
      expr_del(def->var.init);
      free(def->var.init);
    }
    break;
  case DEF_FN:
    del_fn_ast(&def->fn);
    break;
  case DEF_EXT:
    destroy_type_expr(&def->ext.type);
    break;
  default: UNREACHABLE("");
  }
}

void stat_del(Stat *stat)
{
  static_assert(__stat_kind_count == 7, "introduced more stat kinds");
  switch(stat->kind) {
  case STAT_EMPTY:
    // nothing to do;
    break;
  case STAT_ASSIGN:
    if (stat->assign.dst != NULL) {
      expr_del(stat->assign.dst);
      free(stat->assign.dst);
    }
    if (stat->assign.val != NULL) {
      expr_del(stat->assign.val);
      free(stat->assign.val);
    }
    break;
  case STAT_RET:
    if (stat->ret_val) {
      expr_del(stat->ret_val);
      free(stat->ret_val);
    }
    break;
  case STAT_INVOKE:
    if (stat->invoke.fn) {
      expr_del(stat->invoke.fn);
      free(stat->invoke.fn);
    }
    expr_list_del(&stat->invoke.args);
    break;
  case STAT_BLOCK:
    stat_list_del(stat->block);
    break;
  case STAT_IF:
    if (stat->if_else.cond) {
      expr_del(stat->if_else.cond);
      free(stat->if_else.cond);
    }
    if (stat->if_else.on_true) {
      stat_del(stat->if_else.on_true);
      free(stat->if_else.on_true);
    }
    if (stat->if_else.on_false) {
      stat_del(stat->if_else.on_false);
      free(stat->if_else.on_false);
    }
    break;
  case STAT_DEF:
    del_def(&stat->def);
    break;
  default: UNREACHABLE("");
  }
}

static void del_fn_ast(AST_Fn *fn)
{
  destroy_type_expr(&fn->ret_type);
  da_foreach(Def, def, &fn->args) {
    del_def(def);
  }
  da_free(fn->args);
  stat_list_del(fn->body);
}

#endif // MCC_AST_IMPLEMENTATION
