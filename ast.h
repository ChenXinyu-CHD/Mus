#ifndef MCC_AST_H_
#define MCC_AST_H_

#include "3rd/nob.h"

typedef enum {
  AST_ATOM = 0,
  AST_INVOKE,
  AST_BINOP,
  __ast_kind_count
} AST_Kind;

typedef struct AST AST;

typedef struct {
  AST *items;
  size_t count;
  size_t capacity;
} AST_List;

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

struct AST {
  AST_Kind kind;
  union {
    Token atom;
    struct {
      AST* fn;
      AST_List args;
    } invoke;
    struct {
      BinopKind kind;
      AST *lhs;
      AST *rhs;
    } binop;
  };
};

bool compile_expr(Lexer *l, AST *expr);
void ast_del(AST *ast);

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

static AST ast_atom(Token token) { return (AST) {.kind = AST_ATOM, .atom = token}; }
static AST ast_invoke(AST *fn, AST_List args)
{
  return (AST) {
    .kind = AST_INVOKE,
    .invoke =  {
      .fn = fn,
      .args = args,
    }
  };
}
static AST ast_binop(Token op, AST *lhs, AST *rhs)
{
  for (size_t i = 0; i < ARRAY_LEN(binop_list); ++i) {
    if (binop_list[i].token_kind == op.kind) {
      return (AST) {
        .kind = AST_BINOP,
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

static void ast_list_del(AST_List *asts)
{
  da_foreach (AST, ast, asts) {
    ast_del(ast);
  }
  da_free(*asts);
}

void ast_del(AST *ast)
{
  static_assert(__ast_kind_count == 3);
  switch(ast->kind) {
  case AST_ATOM:
    return;
  case AST_INVOKE:
    ast_del(ast->invoke.fn);
    ast_list_del(&ast->invoke.args);
    return;
  case AST_BINOP:
    ast_del(ast->binop.lhs);
    ast_del(ast->binop.rhs);
    return;
  default: UNREACHABLE("");
  }
}

static bool compile_invoke_args(Lexer *l, AST_List *args)
{
  assert(l->current.kind == '(');
  bool result;
  if (!prefetch_not_none(l)) return_defer(false);

  while (l->current.kind != ')') {
    AST arg;
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
  ast_list_del(args);
  return result;
}

static bool compile_simple_expr(Lexer *l, AST *expr)
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
    *expr = ast_atom(l->current);
  }

  if (!lexer_next(l)) return_defer(false);
  while (l->current.kind == '(') {
    AST_List args = {0};
    if (!compile_invoke_args(l, &args)) return_defer(false);

    AST *fn = malloc(sizeof(AST));
    *fn = *expr;
    *expr = ast_invoke(fn, args);
  }

  return true;
 defer:
  ast_del(expr);
  return result;
}

static bool compile_mul(Lexer *l, AST *expr)
{
  if (!compile_simple_expr(l, expr)) return false;

  bool result;
  while (l->current.kind == '*' || l->current.kind == '/' || l->current.kind == '%') {
    Token op = l->current;
    if (!prefetch_not_none(l)) return_defer(false);

    AST rhs_ast = {0};
    if (!compile_simple_expr(l, &rhs_ast)) return_defer(false);

    AST *lhs = malloc(sizeof(AST));
    *lhs = *expr;
    AST *rhs = malloc(sizeof(AST));
    *rhs = rhs_ast;

    *expr = ast_binop(op, lhs, rhs);
  }

  return true;
 defer:
  ast_del(expr);
  return result;
}

static bool compile_add(Lexer *l, AST *expr)
{
  if (!compile_mul(l, expr)) return false;

  bool result;
  while (l->current.kind == '+' || l->current.kind == '-') {
    Token op = l->current;
    if (!prefetch_not_none(l)) return_defer(false);

    AST rhs_ast = {0};
    if (!compile_mul(l, &rhs_ast)) return_defer(false);

    AST *lhs = malloc(sizeof(AST));
    *lhs = *expr;
    AST *rhs = malloc(sizeof(AST));
    *rhs = rhs_ast;

    *expr = ast_binop(op, lhs, rhs);
  }

  return true;
 defer:
  ast_del(expr);
  return result;
}

static bool compile_cmp(Lexer *l, AST *expr)
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

    AST rhs_ast = {0};
    if (!compile_add(l, &rhs_ast)) return_defer(false);

    AST *lhs = malloc(sizeof(AST));
    *lhs = *expr;
    AST *rhs = malloc(sizeof(AST));
    *rhs = rhs_ast;

    *expr = ast_binop(op, lhs, rhs);
  }

  return true;
 defer:
  ast_del(expr);
  return result;
}

bool compile_expr(Lexer *l, AST *expr)
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
