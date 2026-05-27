#ifndef MCC_AST_H_
#define MCC_AST_H_

#include "3rd/nob.h"

#include "SymbolTable.h"

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
  __stat_kind_count,
} Stat_Kind;

typedef struct Stat Stat;

typedef struct {
  Stat *items;
  size_t count;
  size_t capacity;
} Stat_List;

struct Stat {
  Stat_Kind kind;
  Cursor loc;
  union {
    AST_Invoke invoke;
    Expr *ret_val;
    struct {
      Expr *dst;
      Expr *val;
    } assign;
    struct {
      Scoop *local;
      Stat_List stats;
    } block;
    struct {
      Expr *cond;
      Stat *on_true;
      Stat *on_false;
    } if_else;
  };
};

typedef struct Program Program;

bool compile_stat_ast(Lexer *l, Stat *stat, Program *prog, Scoop *sp);
bool compile_block_ast(Lexer *l, Stat *stat, Program *prog, Scoop *outer);
void stat_del(Stat *stat);

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

static Expr expr_atom(Token token) { return (Expr) {.kind = EXPR_ATOM, .atom = token}; }
static Expr expr_invoke(Expr *fn, Expr_List args)
{
  return (Expr) {
    .kind = EXPR_INVOKE,
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

  if (!lexer_next(l)) return_defer(false);
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

bool compile_stat_ast(Lexer *l, Stat *stat, Program *prog, Scoop *sp)
{
  *stat = (Stat) {
    .kind = STAT_EMPTY,
    .loc = l->current.start,
  };

  if (l->current.kind == '{') {
    if (l->current.kind == '{') {
      return compile_block_ast(l, stat, prog, sp);
    }
  }

  bool result = false;
  // simple statement
  if (l->current.kind == TOKEN_IF) {

    stat->kind = STAT_IF;
    if (!prefetch_not_none(l)) return_defer(false);

    stat->if_else.cond = calloc(1, sizeof(Expr));
    if (!compile_expr(l, stat->if_else.cond)) return_defer(false);

    stat->if_else.on_true = calloc(1, sizeof(Stat));
    if (!compile_stat_ast(l, stat->if_else.on_true, prog, sp)) return_defer(false);

    stat->if_else.on_false = NULL;
    if (l->current.kind == TOKEN_ELSE) {
      if (!prefetch_not_none(l)) return_defer(false);
      stat->if_else.on_false = calloc(1, sizeof(Stat));
      if (!compile_stat_ast(l, stat->if_else.on_false, prog, sp)) return_defer(false);
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
    if (!prefetch_not_none(l)) return_defer(false);
  }

  return true;
 defer:
  if (!result) stat_del(stat);
  return result;
}

bool compile_type_expr(Lexer *l, TypeExpr *type);

static bool compile_var(Lexer *l, Stat *init, Scoop *sp)
{
  *init = (Stat) {.kind = STAT_EMPTY};
  if (!prefetch_expect_token(l, TOKEN_ID)) return false;
  Token name = l->current;

  Var *var = calloc(1, sizeof(Var));
  assert(var && "buy more memory");
  *var = (Var) {
    .type = {.kind=TYPE_UNKNOWN},
    .loc = name.start,
  };

  if (!insert_sym(sp, name, SYMBOL_VAR, var)) return false;

  if (!prefetch_not_none(l)) return false;

  if (l->current.kind == ':') {
    if (!prefetch_not_none(l)) return false;
    if (!compile_type_expr(l, &var->type)) return false;
    if (var->type.kind == TYPE_VOID) {
      pcompile_info(l->current.start,
                    "error: the type of a local variable cannot be \"void\"");
      return false;
    }
    if (!prefetch_not_none(l)) return false;
  }

  if (l->current.kind == '=') {
    Cursor loc = l->current.start;
    if (!prefetch_not_none(l)) return false;

    Expr *val = calloc(1, sizeof(*val));
    if (!compile_expr(l, val)) {
      free(val);
      return false;
    }

    Expr *dst = calloc(1, sizeof(*dst));
    *dst = expr_atom(name);

    *init = (Stat) {
      .kind = STAT_ASSIGN,
      .loc = loc,
      .assign = {
        .dst = dst,
        .val = val,
      },
    };
  }
  if (l->current.kind == ';') {
    if (!prefetch_not_none(l)) {
      stat_del(init);
    }
  }
  var->init_end = l->current.start;
  return true;
}

bool compile_def(Lexer *l, Program *prog, Scoop *sp);

bool compile_block_ast(Lexer *l, Stat *stat, Program *prog, Scoop *outer)
{
  assert(l->current.kind == '{');
  if (!prefetch_not_none(l)) return false;

  Scoop *local = malloc(sizeof(*local));
  *local = (Scoop) {
    .upper = outer,
    .symbols = {
      .hasheq = ht_sv_hasheq,
    },
  };

  Stat_List stats = {0};

  while (l->current.kind != '}') {
    Stat s = {0};
    if (l->current.kind == TOKEN_VAR) {
      if (!compile_var(l, &s, local)) return false;
      da_append(&stats, s);
      continue;
    }

    if (compile_def(l, prog, local)) {
      continue;
    }

    if (!compile_stat_ast(l, &s, prog, local)) return false;
    da_append(&stats, s);
  }

  *stat = (Stat) {
    .kind = STAT_BLOCK,
    .block = {
      .local = local,
      .stats = stats
    },
  };
  lexer_next(l);
  return true;
}

static void stat_list_del(Stat_List stats)
{
  da_foreach(Stat, stat, &stats) {
    stat_del(stat);
  }
  da_free(stats);
}

void stat_del(Stat *stat)
{
  static_assert(__stat_kind_count == 6, "introduced more stat kinds");
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
    stat_list_del(stat->block.stats);
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
  default: UNREACHABLE("");
  }
}

#endif // MCC_AST_IMPLEMENTATION

