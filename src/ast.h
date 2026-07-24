#ifndef MCC_AST_H_
#define MCC_AST_H_

#include "3rd_wrapper.h"

#include "type.h"

typedef enum {
  EXPR_INT = 0,
  EXPR_STR,
  EXPR_NAME,
  EXPR_LAMBDA,
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

typedef struct Stat Stat;

typedef struct {
  Stat *items;
  size_t count;
  size_t capacity;
} Stat_List;

typedef struct {
  String_View name;
  Cursor loc;
  TypeExpr type;
} Fn_Arg;

typedef struct {
  bool va;
  Fn_Arg *items;
  size_t count;
  size_t capacity;
} Fn_Arg_List;

typedef struct {
  Cursor loc;

  TypeExpr ret_type;
  Fn_Arg_List args;

  bool is_extern;
  Stat_List body;
} Lambda;

TypeExpr type_of_fn(TypeExpr *ret, Fn_Arg_List *args);

struct Expr {
  Expr_Kind kind;
  Cursor loc;
  TypeExpr type;

  union {
    int64_t integer;
    String_View str;
    String_View name;
    AST_Invoke invoke;
    Lambda lambda;
    struct {
      BinopKind kind;
      Expr *lhs;
      Expr *rhs;
    } binop;
  };
};

Expr *compile_expr(Lexer *l);
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

typedef enum {
  DEF_VAR,
  DEF_LET,
  __def_kind_count,
} Def_Kind;

typedef struct Def Def;

struct Def {
  Def_Kind kind;
  String_View name;
  TypeExpr type;
  Expr *val;
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

Stat *compile_stat(Lexer *l);
bool compile_block(Lexer *l, Stat_List *block);

bool compile_file(Lexer *l, Stat_List *file);

#endif // MCC_AST_H_

#ifdef MCC_AST_IMPLEMENTATION

static_assert(__type_kind_count == 7, "introduced more type kinds");
static struct {
  int token;
  TypeExpr type;
} internal_types[] = {
  {
    .token = TOKEN_VOID,
    .type = { .kind = TYPE_VOID, .size = 0, },
  },
  {
    .token = TOKEN_BOOL,
    .type = { .kind = TYPE_BOOL, .size = 1, },
  },
  {
    .token = TOKEN_U8,
    .type = { .kind = TYPE_UINT, .size = 1, },
  },
  {
    .token = TOKEN_U16,
    .type = { .kind = TYPE_UINT, .size = 2, },
  },
  {
    .token = TOKEN_U32,
    .type = { .kind = TYPE_UINT, .size = 4, },
  },
  {
    .token = TOKEN_U64,
    .type = { .kind = TYPE_UINT, .size = 8, },
  },
  {
    .token = TOKEN_I8,
    .type = { .kind = TYPE_INT, .size = 1, },
  },
  {
    .token = TOKEN_I16,
    .type = { .kind = TYPE_INT, .size = 2, },
  },
  {
    .token = TOKEN_I32,
    .type = { .kind = TYPE_INT, .size = 4, },
  },
  {
    .token = TOKEN_I64,
    .type = { .kind = TYPE_INT, .size = 8, },
  }
};

static bool compile_internal_type(Lexer *l, TypeExpr *type) {
  for (size_t i = 0; i < ARRAY_LEN(internal_types); ++i) {
    if (internal_types[i].token == l->current.kind) {
      *type = type_clone(internal_types[i].type);
      return true;
    }
  }
  return false;
}

static bool compile_type_fn(Lexer *l, TypeExpr *type);

static bool compile_type_expr(Lexer *l, TypeExpr *type)
{
  assert(type != NULL);
  if (compile_internal_type(l, type)) return true;

  switch (l->current.kind) {
  case TOKEN_FN:
    return compile_type_fn(l, type);
  case '&': {
    if (!prefetch_not_none(l)) return false;
    TypeExpr ref_type;
    if (!compile_type_expr(l, &ref_type)) return false;
    *type = type_ptr(ref_type);
    return true;
  }
  default:
    pcompile_info(l->current.start,
                  "error: expected a type but got %s\n",
                  token_name(l->current.kind));
    return false;
  }
}

static bool compile_fn_sign(Lexer *l, TypeExpr *ret, Fn_Arg_List *args)
{
  assert(l->current.kind == TOKEN_FN);

  if (!prefetch_expect_token(l, '(')) return false;

  if (!prefetch_not_none(l)) return false;
  while (l->current.kind != ')') {
    if (l->current.kind == TOKEN_DOTS) { // parse "..." for va_args
      if (!prefetch_expect_token(l, ')')) return false;
      args->va = true;
    } else {
      Fn_Arg arg = {0};

      Lexer forward = *l;
      lexer_next(&forward);
      if (forward.current.kind == ':') {
        if (!expect_token(l, TOKEN_ID)) return false;
        arg.name = l->current.str;
        arg.loc  = l->current.start;

        if (!prefetch_expect_token(l, ':'))   return false;
        if (!prefetch_not_none(l))            return false;
        if (!compile_type_expr(l, &arg.type)) return false;
      } else {
        if (!compile_type_expr(l, &arg.type)) return false;
      }
      da_append(args, arg);

      if (!prefetch_expect_tokens(l, ',', ')')) return false;
      if (l->current.kind == ',') {
        if (!prefetch_not_none(l)) return false;
      }
    }
  }

  if (!prefetch_expect_token(l, TOKEN_ARR)) return false;
  if (!prefetch_not_none(l)) return false;

  if (!compile_type_expr(l, ret)) return false;

  return true;
}

TypeExpr type_of_fn(TypeExpr *ret, Fn_Arg_List *args)
{
  TypeExpr type = {
    .kind = TYPE_FN,
    .fn_type = {
      .ret_type = arena_alloc(sizeof(*ret)),
      .va_args  = args->va,
    }
  };

  *type.fn_type.ret_type = type_clone(*ret);
  da_foreach(Fn_Arg, arg, args) {
    da_append(&type.fn_type.arg_types, arg->type);
  }
  return type;
}

static bool compile_type_fn(Lexer *l, TypeExpr *type)
{
  assert(l->current.kind == TOKEN_FN);

  TypeExpr ret_type = {0};
  Fn_Arg_List args = {0};
  if (!compile_fn_sign(l, &ret_type, &args)) return false;
  *type = type_of_fn(&ret_type, &args);

  return true;
}

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

static Expr *expr_atom(Token token)
{
  Expr *expr = arena_alloc(sizeof(*expr));
  expr->loc  = token.start;

  static_assert(__expr_kind_count == 6, "introduced more expr kinds");
  switch (token.kind) {
  case TOKEN_STR:
    expr->kind = EXPR_STR;
    expr->str  = token.str;
    expr->type = type_ptr(type_int(true, 1));
    return expr;
  case TOKEN_INT:
    expr->kind    = EXPR_INT;
    expr->integer = sv_to_int(token.str);
    expr->type    = type_int(true, 4);
    return expr;
  case TOKEN_TRUE:
    expr->kind    = EXPR_INT;
    expr->integer = 1;
    expr->type    = type_bool();
    return expr;
  case TOKEN_FALSE:
    expr->kind    = EXPR_INT;
    expr->integer = 0;
    expr->type    = type_bool();
    return expr;
  case TOKEN_ID:
    expr->kind = EXPR_NAME;
    expr->name = token.str;
    expr->type.kind = TYPE_UNKNOWN;
    return expr;
  default:
    pcompile_info(token.start,
                  "error: expected an expression but got `"SV_Fmt"`\n",
                  token.str);
    return NULL;
  }
  return expr;
}
static Expr *expr_invoke(Expr *fn, Expr_List args)
{
  Expr *expr = arena_alloc(sizeof(*expr));
  *expr = (Expr) {
    .kind = EXPR_INVOKE,
    .loc = fn->loc,
    .invoke =  {
      .fn = fn,
      .args = args,
    }
  };
  return expr;
}
static Expr *expr_binop(Token op, Expr *lhs, Expr *rhs)
{
  for (size_t i = 0; i < ARRAY_LEN(binop_list); ++i) {
    if (binop_list[i].token_kind == op.kind) {
      Expr *result = arena_alloc(sizeof(*result));
      *result = (Expr) {
        .kind = EXPR_BINOP,
        .loc = op.start,
        .binop = {
          .kind = binop_list[i].binop_kind,
          .lhs = lhs,
          .rhs = rhs,
        }
      };
      return result;
    }
  }
  UNREACHABLE("");
}

static bool compile_invoke_args(Lexer *l, Expr_List *args)
{
  assert(l->current.kind == '(');
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != ')') {
    Expr *arg = compile_expr(l);
    if (arg == NULL) return false;
    da_append(args, *arg);

    if (!expect_tokens(l, ',', ')')) return false;

    if (l->current.kind == ',') {
      if (!prefetch_not_none(l)) return false;
    }
  }
  if (!prefetch_not_none(l)) return false;

  return true;
}

static Expr *compile_lambda(Lexer *l)
{
  Lambda lambda = {.loc = l->current.start};
  if (!compile_fn_sign(l, &lambda.ret_type, &lambda.args)) return NULL;

  if (!prefetch_not_none(l)) return false;
  if (l->current.kind == '{') {
    if (!compile_block(l, &lambda.body)) return false;
    lambda.is_extern = false;
  } else if (l->current.kind == TOKEN_EXT) {
    lambda.is_extern = true;
    lexer_next(l);
  }

  Expr *expr = arena_alloc(sizeof(*expr));
  *expr = (Expr) {
    .kind = EXPR_LAMBDA,
    .loc = lambda.loc,
    .lambda = lambda,
    .type = type_of_fn(&lambda.ret_type, &lambda.args)
  };

  return expr;
}

static Expr *compile_simple_expr(Lexer *l)
{
  Expr *expr = NULL;
  if (l->current.kind == TOKEN_FN) {
    return compile_lambda(l);
  }
  if (l->current.kind == '(') {
    if (!prefetch_not_none(l))  return NULL;
    expr = compile_expr(l);
    if (!expect_token(l, ')'))  return NULL;
  } else {
    expr = expr_atom(l->current);
  }

  if (expr == NULL) return NULL;

  if (!prefetch_not_none(l)) return NULL;
  while (l->current.kind == '(') {
    Expr_List args = {0};
    if (!compile_invoke_args(l, &args)) return NULL;
    expr = expr_invoke(expr, args);
  }

  return expr;
}

static Expr *compile_mul(Lexer *l)
{
  Expr *expr = compile_simple_expr(l);
  if (expr == NULL) return NULL;
  while (l->current.kind == '*' || l->current.kind == '/' || l->current.kind == '%') {
    Token op = l->current;
    if (!prefetch_not_none(l)) return NULL;

    Expr *rhs = compile_simple_expr(l);
    if (rhs == NULL) return NULL;

    expr = expr_binop(op, expr, rhs);
  }

  return expr;
}

static Expr *compile_add(Lexer *l)
{
  Expr *expr = compile_mul(l);
  if (expr == NULL) return NULL;

  while (l->current.kind == '+' || l->current.kind == '-') {
    Token op = l->current;
    if (!prefetch_not_none(l)) return NULL;

    Expr *rhs = compile_mul(l);
    if (rhs == NULL) return NULL;

    expr = expr_binop(op, expr, rhs);
  }

  return expr;
}

static Expr *compile_cmp(Lexer *l)
{
  Expr *expr = compile_add(l);
  if (expr == NULL) return NULL;

  while (l->current.kind == TOKEN_EQ ||
         l->current.kind == TOKEN_LE ||
         l->current.kind == TOKEN_GE ||
         l->current.kind == '<' ||
         l->current.kind == '>' ||
         l->current.kind == TOKEN_NEQ) {
    Token op = l->current;
    if (!prefetch_not_none(l)) return NULL;

    Expr *rhs = compile_add(l);
    if (rhs == NULL) return NULL;

    expr = expr_binop(op, expr, rhs);
  }

  return expr;
}

Expr *compile_expr(Lexer *l)
{
  // EXPR   :: CMP
  // CMP    :: ADD | ADD == CMP | ADD != CMP | ADD < CMP | ADD > CMP | ADD <= CMP | ADD >= CMP
  // ADD    :: MUL | MUL + ADD | MUL - ADD
  // MUL    :: SIMPLE | SIMPLE * MUL | SIMPLE / MUL | SIMPLE % MUL
  // SIMPLE :: ATOM | INVOKE | ( EXPR )
  // ATOM   :: STR | INT | ID
  // INVOKE :: EXPR ( ARGS )
  // ARGS   :: EXPR | EXPR , ARGS
  return compile_cmp(l);
}

static Stat *compile_def(Lexer *l, Def_Kind kind)
{
  assert(l->current.kind == (kind == DEF_LET ? TOKEN_LET : TOKEN_VAR));
  if (!prefetch_not_none(l)) return NULL;

  Stat *stat = arena_calloc(1, sizeof(*stat));
  stat->kind = STAT_DEF;
  stat->def.kind = kind;
  stat->loc  = l->current.start;

  if (!expect_token(l, TOKEN_ID)) return NULL;
  stat->def.name = l->current.str;

  if (!prefetch_not_none(l)) return NULL;

  if (l->current.kind == ':') {
    if (!prefetch_not_none(l)) return NULL;
    if (!compile_type_expr(l, &stat->def.type)) return NULL;

    if (stat->def.type.kind == TYPE_VOID) {
      pcompile_info(l->current.start,
                    "error: the type of a symbol cannot be \"void\"");
      return NULL;
    }
    if (!prefetch_not_none(l)) return NULL;
  }

  if (l->current.kind == '=') {
    if (!prefetch_not_none(l)) return NULL;

    stat->def.val = compile_expr(l);
    if (stat->def.val == NULL) {
      return NULL;
    }
  }

  return stat;
}

Stat *compile_stat(Lexer *l)
{
  Stat *stat = NULL;
  Cursor loc = l->current.start;

  if (l->current.kind == '{') {
    stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_BLOCK;
    stat->loc  = loc;

    if (!compile_block(l, &stat->block)) return NULL;
    return stat;
  }

  // simple statement
  if (l->current.kind == TOKEN_LET) {
    stat = compile_def(l, DEF_LET);
    if (stat == NULL) return NULL;
  } else if (l->current.kind == TOKEN_VAR) {
    stat = compile_def(l, DEF_VAR);
    if (stat == NULL) return NULL;
  } else if (l->current.kind == TOKEN_IF) {
    if (!prefetch_not_none(l)) return NULL;

    stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_IF;

    stat->if_else.cond = compile_expr(l);
    if (stat->if_else.cond == NULL) return NULL;

    stat->if_else.on_true = compile_stat(l);
    if (stat->if_else.on_true == NULL) return NULL;

    stat->if_else.on_false = NULL;
    if (l->current.kind == TOKEN_ELSE) {
      if (!prefetch_not_none(l)) return NULL;
      stat->if_else.on_false = compile_stat(l);
      if (stat->if_else.on_false == NULL) return NULL;
    }
  } else if (l->current.kind == TOKEN_RET) {
    if (!prefetch_not_none(l)) return NULL;

    stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_RET;

    stat->ret_val = compile_expr(l);
    if (stat->ret_val == NULL) return NULL;
  } else {
    Expr *expr = compile_expr(l);
    if (expr == NULL) return NULL;

    if (l->current.kind == '=') {
      stat = arena_alloc(sizeof(*stat));
      stat->kind = STAT_ASSIGN;
      stat->assign.dst = expr;

      if (!prefetch_not_none(l)) return NULL;
      stat->assign.val = compile_expr(l);
      if (stat->assign.val == NULL) return NULL;
    } else if (expr->kind == EXPR_INVOKE) {
      stat = arena_alloc(sizeof(*stat));
      stat->kind = STAT_INVOKE;
      stat->invoke = expr->invoke;
    } else {
      pcompile_info(stat->loc,
                    "error: expect a statement, but got an expression.\n");
      return NULL;
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

  stat->loc = loc;
  return stat;
}

bool compile_block(Lexer *l, Stat_List *block)
{
  assert(l->current.kind == '{');
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != '}') {
    Stat *s = compile_stat(l);
    if (s == NULL) return false;
    da_append(block, *s);
  }
  assert(l->current.kind == '}');
  lexer_next(l);
  return true;
}

bool compile_file(Lexer *l, Stat_List *stats)
{
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != TOKEN_EOF && l->current.kind != TOKEN_ERR) {
    Stat *s = compile_stat(l);
    if (s == NULL) return false;
    da_append(stats, *s);
  }
  return l->current.kind == TOKEN_EOF;
}

#endif // MCC_AST_IMPLEMENTATION
