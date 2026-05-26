#ifndef MCC_AST_H_
#define MCC_AST_H_

#include "3rd/nob.h"

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

struct Expr {
  Expr_Kind kind;
  union {
    Token atom;
    struct {
      Expr* fn;
      Expr_List args;
    } invoke;
    struct {
      BinopKind kind;
      Expr *lhs;
      Expr *rhs;
    } binop;
  };
};

bool compile_expr(Lexer *l, Expr *expr);
void expr_del(Expr *expr);

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
static_assert(__binop_kind_count == ARRAY_LEN(binop_list));

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
  static_assert(__expr_kind_count == 3);
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

#endif // MCC_AST_IMPLEMENTATION
