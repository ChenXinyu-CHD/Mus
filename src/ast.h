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
  EXPR_REF,
  EXPR_DREF,
  EXPR_ARR_INIT,
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
    struct {
      bool mutable;
      Expr *inner;
    } ref;
    Expr *deref;
    struct {
      Expr *items;
      size_t count;
      size_t capacity;
      // repeate is a compiletime value
      // if it is not NULL, items will contain
      // only one element.
      Expr *repeat;
    } arr_init;
  };
};

bool compile_expr(Lexer *l, Expr *result);
typedef enum {
  STAT_EMPTY = 0,
  STAT_INVOKE,
  STAT_RET,
  STAT_ASSIGN,
  STAT_BLOCK,
  STAT_IF,
  STAT_FOR,
  STAT_BREAK,
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
  bool is_extern;
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
    struct {
      Stat *init;
      Expr *cond;
      Stat *update;
      Stat *body;
    } for_loop;
  };
};

Stat *compile_stat(Lexer *l, bool breakable);
bool compile_block(Lexer *l, Stat_List *block, bool breakable);

bool compile_file(Lexer *l, Stat_List *file);

#endif // MCC_AST_H_

#ifdef MCC_AST_IMPLEMENTATION

static_assert(__type_kind_count == 8, "introduced more type kinds");
static struct {
  int token;
  TypeExpr type;
} internal_types[] = {
  {
    .token = TOKEN_VOID,
    .type = type_void(),
  },
  {
    .token = TOKEN_BOOL,
    .type = type_bool(),
  },
  {
    .token = TOKEN_U8,
    .type = type_int(TYPE_UINT, 1),
  },
  {
    .token = TOKEN_U16,
    .type = type_int(TYPE_UINT, 2),
  },
  {
    .token = TOKEN_U32,
    .type = type_int(TYPE_UINT, 4),
  },
  {
    .token = TOKEN_U64,
    .type = type_int(TYPE_UINT, 8),
  },
  {
    .token = TOKEN_I8,
    .type = type_int(TYPE_INT, 1),
  },
  {
    .token = TOKEN_I16,
    .type = type_int(TYPE_INT, 2),
  },
  {
    .token = TOKEN_I32,
    .type = type_int(TYPE_INT, 4),
  },
  {
    .token = TOKEN_I64,
    .type = type_int(TYPE_INT, 8),
  }
};

static bool compile_internal_type(Lexer *l, TypeExpr *type) {
  for (size_t i = 0; i < ARRAY_LEN(internal_types); ++i) {
    if (internal_types[i].token == l->current.kind) {
      *type = internal_types[i].type;
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
    bool mutable = false;
    if (l->current.kind == TOKEN_MUT) {
      if (!prefetch_not_none(l)) return false;
      mutable = true;
    }
    TypeExpr inner;
    if (!compile_type_expr(l, &inner)) return false;
    *type = type_ptr(inner, mutable);
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
  TypeList arg_types = {0};
  da_foreach(Fn_Arg, arg, args) {
    da_append(&arg_types, arg->type);
  }
  return type_fn(*ret, arg_types, args->va);
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

static bool expr_atom(Token token, Expr *result)
{
  result->loc  = token.start;

  static_assert(__expr_kind_count == 9, "introduced more expr kinds");
  switch (token.kind) {
  case TOKEN_STR:
    result->kind = EXPR_STR;
    result->str  = token.str;
    result->type = type_ptr(type_int(TYPE_INT, 1), false);
    return true;
  case TOKEN_INT:
    result->kind    = EXPR_INT;
    result->integer = sv_to_int(token.str);
    result->type    = type_unknown();
    return true;
  case TOKEN_TRUE:
    result->kind    = EXPR_INT;
    result->integer = 1;
    result->type    = type_bool();
    return true;
  case TOKEN_FALSE:
    result->kind    = EXPR_INT;
    result->integer = 0;
    result->type    = type_bool();
    return true;
  case TOKEN_ID:
    result->kind = EXPR_NAME;
    result->name = token.str;
    result->type.kind = TYPE_UNKNOWN;
    return true;
  default:
    pcompile_info(token.start,
                  "error: expected an expression but got `"SV_Fmt"`\n",
                  token.str);
    return false;
  }
}

static bool compile_invoke_args(Lexer *l, Expr_List *args)
{
  assert(l->current.kind == '(');
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != ')') {
    Expr arg = {0};
    if (!compile_expr(l, &arg)) return false;
    da_append(args, arg);

    if (!expect_tokens(l, ',', ')')) return false;

    if (l->current.kind == ',') {
      if (!prefetch_not_none(l)) return false;
    }
  }
  if (!prefetch_not_none(l)) return false;

  return true;
}

static bool compile_lambda(Lexer *l, Expr *result)
{
  Lambda lambda = {.loc = l->current.start};
  if (!compile_fn_sign(l, &lambda.ret_type, &lambda.args)) return false;

  if (!prefetch_expect_token(l, '{')) return false;

  if (!compile_block(l, &lambda.body, false)) return false;

  *result = (Expr) {
    .kind = EXPR_LAMBDA,
    .loc = lambda.loc,
    .lambda = lambda,
    .type = type_of_fn(&lambda.ret_type, &lambda.args)
  };

  return true;
}

static bool compile_simple_expr(Lexer *l, Expr *result)
{
  if (l->current.kind == TOKEN_FN) {
    return compile_lambda(l, result);
  }
  if (l->current.kind == '(') {
    if (!prefetch_not_none(l))    return false;
    if (!compile_expr(l, result)) return false;
    if (!expect_token(l, ')'))    return false;
    if (!prefetch_not_none(l))    return false;
  } else if (l->current.kind == '&') {
    Cursor loc = l->current.start;
    if (!prefetch_not_none(l))  return false;
    bool mutable = false;
    if (l->current.kind == TOKEN_MUT) {
      if (!prefetch_not_none(l)) return false;
      mutable = true;
    }
    Expr inner = {0};
    if (!compile_simple_expr(l, &inner)) return false;
    if (inner.kind != EXPR_NAME && inner.kind != EXPR_DREF) {
      pcompile_info(inner.loc, "error: this cannot be referenced.\n");
      return false;
    }

    result->loc = loc;
    result->kind = EXPR_REF;
    result->ref.mutable = mutable;
    result->ref.inner = arena_copy(inner);
  } else if (l->current.kind == '*') {
    Cursor loc = l->current.start;
    if (!prefetch_not_none(l))  return false;
    Expr inner = {0};
    if (!compile_simple_expr(l, &inner)) return false;

    result->loc = loc;
    result->kind  = EXPR_DREF;
    result->deref = arena_copy(inner);
  } else if (l->current.kind == '[') {
    TODO("");
  } else {
    if (!expr_atom(l->current, result)) return false;
    if (!prefetch_not_none(l))          return false;
  }
  return true;
}

static bool compile_invoke(Lexer *l, Expr *result) {
  if (!compile_simple_expr(l, result)) return false;
  while (l->current.kind == '(') {
    Expr fn = *result;
    Expr_List args = {0};
    if (!compile_invoke_args(l, &args)) return false;
    result->kind = EXPR_INVOKE;
    result->invoke.fn = arena_copy(fn);
    result->invoke.args = args;
  }

  return true;
}

typedef struct {
  int token_kind;
  BinopKind binop_kind;
} Binop_Msg;

static const Binop_Msg binop_level_mul[] = {
  {.token_kind =  '*',       .binop_kind = BINOP_MUL,},
  {.token_kind =  '/',       .binop_kind = BINOP_DIV,},
  {.token_kind =  '%',       .binop_kind = BINOP_MOD,},
};

static const Binop_Msg binop_level_add[] = {
  {.token_kind =  '+',       .binop_kind = BINOP_ADD,},
  {.token_kind =  '-',       .binop_kind = BINOP_SUB,},
};

static const Binop_Msg binop_level_cmp[] = {
  {.token_kind =  '<',       .binop_kind = BINOP_LS,},
  {.token_kind =  '>',       .binop_kind = BINOP_GT,},
  {.token_kind =  TOKEN_EQ,  .binop_kind = BINOP_EQ,},
  {.token_kind =  TOKEN_NEQ, .binop_kind = BINOP_NEQ,},
  {.token_kind =  TOKEN_LE,  .binop_kind = BINOP_LE,},
  {.token_kind =  TOKEN_GE,  .binop_kind = BINOP_GE,},
};

// a little bit macro magic for static_assert

#define BINOP_LEVEL_MAPPER(X)                   \
  X(binop_level_cmp)                            \
    X(binop_level_add)                          \
    X(binop_level_mul)                          \

#define GEN_BINOP_LEVEL(X)                      \
  { .items = (X), .count = ARRAY_LEN(X) },

static const struct {
  const Binop_Msg *items;
  size_t count;
} binop_level[] = {
  BINOP_LEVEL_MAPPER(GEN_BINOP_LEVEL)
};
#undef GEN_BINOP_LEVEL

#define BINOP_COUNT 0 BINOP_LEVEL_MAPPER(BINOP_COUNT_IMPL)
#define BINOP_COUNT_IMPL(X) + ARRAY_LEN(X)

static_assert(__binop_kind_count == BINOP_COUNT,
              "introduced more binop kinds");
#undef BINOP_COUNT
#undef BINOP_COUNT_IMPL

const char *binop_name(BinopKind kind)
{
  for (size_t i = 0; i < ARRAY_LEN(binop_level); ++i) {
    da_foreach(const Binop_Msg, msg, &binop_level[i]) {
      if (msg->binop_kind == kind) {
        return token_name(msg->token_kind);
      }
    }
  }
  UNREACHABLE("");
}

static bool compile_binop(Lexer *l, Expr *result, size_t level) {
  assert(level <= ARRAY_LEN(binop_level));
  if (level == ARRAY_LEN(binop_level)) {
    return compile_invoke(l, result);
  }
  if (!compile_binop(l, result, level + 1)) return false;
  while (true) {
    const Binop_Msg *match = NULL;
    da_foreach(const Binop_Msg, msg, &binop_level[level]) {
      if (l->current.kind == msg->token_kind) {
        match = msg;
        break;
      }
    }
    if (match == NULL) break;

    if (!prefetch_not_none(l)) return false;

    Expr lhs = *result;
    Expr rhs = {0};
    if (!compile_binop(l, &rhs, level + 1)) return false;

    result->kind = EXPR_BINOP;
    result->loc  = lhs.loc;
    result->binop.kind = match->binop_kind;
    result->binop.lhs  = arena_copy(lhs);
    result->binop.rhs  = arena_copy(rhs);
  }
  return true;
}

bool compile_expr(Lexer *l, Expr *result)
{
  // EXPR   :: CMP
  // CMP    :: ADD | ADD == CMP | ADD != CMP | ADD < CMP | ADD > CMP | ADD <= CMP | ADD >= CMP
  // ADD    :: MUL | MUL + ADD | MUL - ADD
  // MUL    :: SIMPLE | SIMPLE * MUL | SIMPLE / MUL | SIMPLE % MUL
  // SIMPLE :: ATOM | INVOKE | ( EXPR )
  // ATOM   :: STR | INT | ID
  // INVOKE :: EXPR ( ARGS )
  // ARGS   :: EXPR | EXPR , ARGS
  return compile_binop(l, result, 0);
}

Expr *lambda_of_type(TypeExpr *type, Cursor loc)
{
  Expr *expr = arena_calloc(1, sizeof(*expr));
  expr->type = *type;
  expr->kind = EXPR_LAMBDA;
  expr->loc  = loc;

  expr->lambda.ret_type = *type->fn.ret;
  expr->lambda.args.va  = type->fn.va_args;
  expr->lambda.loc      = loc;

  da_foreach(TypeExpr, arg_type, &type->fn.args) {
    Fn_Arg arg = { .type = *arg_type };
    da_append(&expr->lambda.args, arg);
  }
  return expr;
}

static Stat *compile_def(Lexer *l)
{
  Def_Kind kind;
  if (l->current.kind == TOKEN_LET) {
    kind = DEF_LET;
  } else if (l->current.kind == TOKEN_VAR) {
    kind = DEF_VAR;
  } else {
    UNREACHABLE("this statement is not a def. It must be checked outside.");
  }
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

    Expr val = {0};
    if (!compile_expr(l, &val)) return false;
    stat->def.val = arena_copy(val);
  } else if (l->current.kind == TOKEN_EXT) {
    lexer_next(l);
    stat->def.is_extern = true;
  }

  return stat;
}

static Stat *compile_if_else(Lexer *l, bool breakable)
{
  assert(l->current.kind == TOKEN_IF);
  Cursor loc = l->current.start;
  if (!prefetch_not_none(l)) return NULL;

  Stat *stat = arena_alloc(sizeof(*stat));
  stat->kind = STAT_IF;
  stat->loc  = loc;

  Expr cond = {0};
  if (!compile_expr(l, &cond)) return NULL;
  stat->if_else.cond = arena_copy(cond);

  stat->if_else.on_true = compile_stat(l, breakable);
  if (stat->if_else.on_true == NULL) return NULL;

  stat->if_else.on_false = NULL;
  if (l->current.kind == TOKEN_ELSE) {
    if (!prefetch_not_none(l)) return NULL;
    stat->if_else.on_false = compile_stat(l, breakable);
    if (stat->if_else.on_false == NULL) return NULL;
  }
  return stat;
}

static Stat *compile_simple_stat(Lexer *l)
{
  Cursor loc = l->current.start;
  Expr expr = {0};
  if (!compile_expr(l, &expr)) return NULL;

  if (l->current.kind == '=') {
    Stat *stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_ASSIGN;
    stat->loc  = loc;
    stat->assign.dst = arena_copy(expr);

    if (!prefetch_not_none(l)) return NULL;
    Expr val = {0};
    if (!compile_expr(l, &val)) return NULL;
    stat->assign.val = arena_copy(val);
    return stat;
  } else if (expr.kind == EXPR_INVOKE) {
    Stat *stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_INVOKE;
    stat->loc  = loc;
    stat->invoke = expr.invoke;
    return stat;
  } else {
    pcompile_info(l->current.start,
                  "error: unexpected stuff in a statement.\n");
    return NULL;
  }
}

static Stat *empty_stat(Cursor loc)
{
  Stat *stat = arena_calloc(1, sizeof(*stat));
  stat->kind = STAT_EMPTY;
  stat->loc  = loc;
  return stat;
}

static Stat *compile_loop_init(Lexer *l)
{
  Stat *stat = NULL;
  if (l->current.kind == ';') {
    stat = empty_stat(l->current.start);
  } else if (l->current.kind == TOKEN_VAR) {
    stat = compile_def(l);
  } else {
    stat = compile_simple_stat(l);
  }
  if (stat != NULL) {
    if (!expect_token(l, ';')) return NULL;
    if (!prefetch_not_none(l)) return NULL;
  }
  return stat;
}

static bool compile_loop_cond(Lexer *l, Expr *expr)
{
  if (l->current.kind == ';') {
    Token fake_token = {.kind = TOKEN_TRUE, .start = l->current.start};
    if (!expr_atom(fake_token, expr)) return false;
  } else {
    if (!compile_expr(l, expr))       return false;
  }
  if (!expect_token(l, ';'))          return false;
  if (!prefetch_not_none(l))          return false;
  return true;
}

static Stat *compile_loop_update(Lexer *l)
{
  if (l->current.kind == '{') {
    return empty_stat(l->current.start);
  } else {
    return compile_simple_stat(l);
  }
}

static Stat *compile_loops(Lexer *l)
{
  Token token = l->current;
  assert(token.kind == TOKEN_FOR  ||
         token.kind == TOKEN_LOOP ||
         token.kind == TOKEN_WHILE);
  if (!prefetch_not_none(l)) return NULL;
  Stat *stat = arena_alloc(sizeof(*stat));

  stat->kind = STAT_FOR;
  if (token.kind == TOKEN_FOR) {
    stat->for_loop.init = compile_loop_init(l);
    if (stat->for_loop.init == NULL) return NULL;

    Expr cond = {0};
    if (!compile_loop_cond(l, &cond)) return NULL;
    stat->for_loop.cond = arena_copy(cond);

    stat->for_loop.update = compile_loop_update(l);
    if (stat->for_loop.update == NULL) return NULL;

    stat->for_loop.body = compile_stat(l, true);
    if (stat->for_loop.body == NULL) return NULL;
  } else if (token.kind == TOKEN_WHILE) {
    Expr cond = {0};
    if (!compile_expr(l, &cond)) return NULL;
    stat->for_loop.cond = arena_copy(cond);

    stat->for_loop.body = compile_stat(l, true);
    if (stat->for_loop.body == NULL) return NULL;
  } else {
    stat->for_loop.body = compile_stat(l, true);
    if (stat->for_loop.body == NULL) return NULL;
  }
  stat->loc = token.start;
  return stat;
}

Stat *compile_stat(Lexer *l, bool breakable)
{
  Stat *stat = NULL;
  Token token = l->current;

  // empty statment is allowed
  if (token.kind == ';') {
    lexer_next(l);
    return empty_stat(l->current.start);
  }

  if (token.kind == '{') {
    stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_BLOCK;
    stat->loc  = token.start;

    if (!compile_block(l, &stat->block, breakable)) return NULL;
    return stat;
  } else if (token.kind == TOKEN_IF) {
    return compile_if_else(l, breakable);
  } else if (token.kind == TOKEN_FOR || token.kind == TOKEN_WHILE || token.kind == TOKEN_LOOP) {
    return compile_loops(l);
  }

  // simple statement
  if (token.kind == TOKEN_LET || token.kind == TOKEN_VAR) {
    stat = compile_def(l);
    if (stat == NULL) return NULL;
  } else if (token.kind == TOKEN_RET) {
    if (!prefetch_not_none(l)) return NULL;

    stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_RET;
    stat->loc = token.start;

    if (l->current.kind == ';') {
      stat->ret_val = NULL;
    } else {
      Expr ret = {0};
      if (!compile_expr(l, &ret)) return NULL;
      stat->ret_val = arena_copy(ret);
    }
  } else if (token.kind == TOKEN_BREAK) {
    if (!breakable) {
      pcompile_info(token.start, "error: `break` is not allowed here.\n");
    }
    if (!prefetch_not_none(l)) return NULL;
    stat = arena_alloc(sizeof(*stat));
    stat->kind = STAT_BREAK;
    stat->loc  = token.start;
  } else {
    stat = compile_simple_stat(l);
  }

  // a simple statment must be followed with an optional ';'
  if (!expect_token(l, ';')) return false;
  lexer_next(l);
  return stat;
}

bool compile_block(Lexer *l, Stat_List *block, bool breakable)
{
  assert(l->current.kind == '{');
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != '}') {
    Stat *s = compile_stat(l, breakable);
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
    Stat *s = compile_stat(l, false);
    if (s == NULL) return false;
    da_append(stats, *s);
  }
  return l->current.kind == TOKEN_EOF;
}

#endif // MCC_AST_IMPLEMENTATION
