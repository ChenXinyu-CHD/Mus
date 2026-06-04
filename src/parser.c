#include "parser.h"

#include "nob.h"
#include "ht.h"

#include "lexer.h"
#include "utils.h"
#include "type.h"
#include "ast.h"

typedef enum {
  SYMBOL_FN = 0,
  SYMBOL_VAR,
  SYMBOL_EXTERN,
  __symbol_kind_count,
} SymbolKind;

typedef struct {
  SymbolKind kind;
  Cursor loc;
  union {
    Var *var;
    Extern *ext;
    Fn *fn;
    void *ptr;
  };
} Symbol;

typedef struct Scoop Scoop;
struct Scoop {
  Scoop *upper;
  Ht(String_View, Symbol) symbols;
};

static Scoop *new_scoop(Scoop *upper)
{
  Scoop *s = calloc(1, sizeof(*s));
  s->symbols.hasheq = ht_sv_hasheq;
  s->upper = upper;
  return s;
}

static Symbol *insert_sym(Scoop *sp, String_View name, Cursor loc, SymbolKind kind)
{
  Symbol *sym = ht_find(&sp->symbols, name);
  if (sym != NULL) {
    pcompile_info(loc,
                  "error: symbol "SV_Fmt" redefined in this scoop\n",
                  SV_Arg(name));
    // TODO: report where the symbol is first defined;
    return NULL;
  } else {
    sym = ht_put(&sp->symbols, name);
    *sym = (Symbol) {
      .kind = kind,
      .loc = loc,
    };
    return sym;
  }
}

// C is so bad
typedef struct {
  Scoop *scoop;
  Symbol *sym;
} SymSearchResult;

static SymSearchResult sym_search(Scoop *sp, String_View name)
{
  for (Scoop *s = sp; s != NULL; s = s->upper) {
    Symbol *sym = ht_find(&s->symbols, name);
    if (sym != NULL) {
      return (SymSearchResult) {
        .scoop = s,
        .sym = sym,
      };
    }
  }

  return (SymSearchResult) {NULL, NULL};
}

Var *alloc_var(VarList *vars)
{
  Var *var = malloc(sizeof(Var));
  *var = (Var) {
    .type = {.kind = TYPE_UNKNOWN},
  };
  da_append(vars, var);
  return var;
}
static void destroy_op(Op *op)
{
  static_assert(__op_kind_count == 7, "introduced more op kinds");
  switch (op->kind) {
  case OP_INVOKE:
    da_free(op->invoke.args);
    break;
  case OP_RETURN:
  case OP_SET_VAR:
  case OP_BINOP:
  case OP_JMP_ELSE:
  case OP_JMP:
  case OP_LABEL:
    break;
  default: UNREACHABLE("destroy op");
  }
}

static void destroy_fn(Fn *fn)
{
  if (fn->fn_body.count != 0) {
    da_foreach (Op, op, &fn->fn_body) {
      destroy_op(op);
    }
    da_free(fn->fn_body);
  }

  if (fn->vars.count != 0) {
    for (size_t i = 0; i < fn->vars.count; ++i) {
      Var *var = fn->vars.items[i];
      destroy_type_expr(&var->type);
      free(var);
    }
    da_free(fn->vars);
  }
}

static void destroy_fn_list(FnList *fn_list)
{
  for (size_t i = 0; i < fn_list->count; ++i) {
    Fn *fn = fn_list->items[i];
    destroy_fn(fn);
    free(fn);
  }
  da_free(*fn_list);
}

static void dump_arg(String_Builder *sb, Arg *arg)
{
  static_assert(__arg_kind_count == 6, "introduced more arg kinds");
  switch(arg->kind) {
  case ARG_NONE:
    sb_appendf(sb, "None");
    break;
  case ARG_EXTERN:
    sb_appendf(sb, SV_Fmt, SV_Arg(arg->ext->linkname));
    break;
  case ARG_FN:
    sb_appendf(sb, SV_Fmt, SV_Arg(sb_to_sv(arg->fn->name)));
    break;
  case ARG_VAR:
    sb_appendf(sb, "var[%ld]", arg->var->id);
    break;
  case ARG_LIT_INT:
    sb_appendf(sb, "%d", arg->num_int);
    break;
  case ARG_LIT_STR:
    sb_appendf(sb, ".S_%ld", arg->str_label);
    break;
  default: UNREACHABLE("");
  }
}

void dump_op(String_Builder *sb, Op *op)
{
  static_assert(__op_kind_count == 7, "introduced more op kinds");
  switch (op->kind) {
  case OP_INVOKE:
    if (!op->invoke.ret_ignore) {
      dump_arg(sb, &op->invoke.ret);
      sb_appendf(sb, " = ");
    }
    dump_arg(sb, &op->invoke.fn);
    sb_appendf(sb, "(");
    if (op->invoke.args.count != 0) {
      dump_arg(sb, &da_first(&op->invoke.args));
      for (size_t i = 1; i < op->invoke.args.count; ++i) {
        sb_appendf(sb, ", ");
        dump_arg(sb, &op->invoke.args.items[i]);
      }
    }
    sb_appendf(sb, ")");
    break;
  case OP_RETURN:
    sb_appendf(sb, "ret ");
    dump_arg(sb, &op->ret_val);
    break;
  case OP_SET_VAR:
    dump_arg(sb, &op->set_var.var);
    sb_appendf(sb, " = ");
    dump_arg(sb, &op->set_var.var);
    break;
  case OP_BINOP:
    dump_arg(sb, &op->binop.dst);
    sb_appendf(sb, " = ");
    dump_arg(sb, &op->binop.lhs);
    sb_appendf(sb, " %s ", binop_name(op->binop.kind));
    dump_arg(sb, &op->binop.rhs);
    break;
  case OP_JMP:
    sb_appendf(sb, "jmp .%ld", op->jmp.label);
    break;
  case OP_JMP_ELSE:
    sb_appendf(sb, "jmp_else ");
    dump_arg(sb, &op->jmp.cond);
    sb_appendf(sb, ", .%ld", op->jmp.label);
    break;
  case OP_LABEL:
    sb_appendf(sb, ".%ld:", op->label);
    break;
  default: UNREACHABLE("");
  }
  sb_appendf(sb, "\n");
}

static size_t compile_strlit(Program *prog, String_View str)
{
  size_t str_count = prog->str_lits.count;
  for (size_t i = 0; i < str_count; ++i) {
    if (sv_eq(prog->str_lits.items[i], str)) {
      return i;
    }
  }

  da_append(&prog->str_lits, str);
  return str_count;
}

static bool contains(void *arr, size_t n, void *val)
{
  void **p = arr;
  for (size_t i = 0; i < n; ++i) {
    if (val == p[i]) return true;
  }
  return false;
}

typedef struct {
  AST_Fn *fn;
  Scoop *sp;
} Fn_Ctx;

typedef Ht(Fn*, Fn_Ctx) Known_Fn;

typedef struct {
  Program *prog;
  Fn *fn;

  FnList ungenerated;
  Known_Fn known;

  struct {
    Scoop **items;
    size_t capacity;
    size_t count;
  } sps;
} Gen_Context;

static Fn *push_fn_ast(Gen_Context *ctx, String_Builder name, AST_Fn* ast, Scoop *sp)
{
  Fn *fn = calloc(1, sizeof(*fn));
  fn->name = name;
  da_append(&ctx->ungenerated, fn);

  sp = new_scoop(sp);
  da_append(&ctx->sps, sp);

  *ht_put(&ctx->known, fn) = (Fn_Ctx) {
    .fn = ast,
    .sp = sp,
  };
  return fn;
}

static bool id_to_arg(String_View name, Cursor loc, Arg *arg, Scoop *sp, Gen_Context *ctx)
{
  arg->loc = loc;
  SymSearchResult r = sym_search(sp, name);
  if (r.scoop == NULL) {
    pcompile_info(loc,
                  "error: cannot find `"SV_Fmt"` in this scoop\n",
                  SV_Arg(name));
    return false;
  }

  static_assert(__symbol_kind_count == 3, "introduced more symbol kinds");
  switch (r.sym->kind) {
  case SYMBOL_VAR: {
    Var* var = r.sym->var;

    if (!contains(ctx->fn->vars.items, ctx->fn->vars.count, var)) {
      // TODO: support global variables
      pcompile_info(arg->loc,
                    "error: try to visit a nonlocal variable\n");
      pcompile_info(r.sym->loc,
                    "info: `"SV_Fmt"` is defined in here\n",
                    SV_Arg(name));
      return false;
    }
    arg->kind = ARG_VAR;
    arg->var = var;
    arg->type = type_clone(var->type);
    return true;
  }
  case SYMBOL_EXTERN: {
    Extern *ext = r.sym->ext;
    arg->kind = ARG_EXTERN;
    arg->ext = ext;
    arg->type = type_clone(ext->type);
    return true;
  }
  case SYMBOL_FN: {
    arg->kind = ARG_FN;
    arg->fn = r.sym->fn;
    // this must be unknown, because the refered function may not be generated
    arg->type.kind = TYPE_UNKNOWN;
    return true;
  }
  default: UNREACHABLE("");
  }
}

static bool expr_to_ir(Expr *expr, Scoop *sp, Gen_Context *ctx);

static bool expr_to_arg(Expr *expr, Scoop *sp, Gen_Context *ctx, Arg *result)
{
  static_assert(__expr_kind_count == 3, "introduced more expr kinds");
  switch (expr->kind) {
  case EXPR_ATOM: {
    Token token = expr->atom;
    switch (token.kind) {
    case TOKEN_ID:
      return id_to_arg(token.str, token.start, result, sp, ctx);
    case TOKEN_STR:
      result->kind = ARG_LIT_STR;
      result->str_label = compile_strlit(ctx->prog, token.str);
      result->type = type_ptr((TypeExpr) {
          .kind = TYPE_INT,
          .size = 1,
        });
      return true;
    case TOKEN_INT:
      result->kind = ARG_LIT_INT;
      result->num_int = sv_to_int(token.str);
      result->type = (TypeExpr) {
        .kind = TYPE_INT,
        .size = 4,
      };
      return true;
    case TOKEN_TRUE:
      result->kind = ARG_LIT_INT;
      result->num_int = 1;
      result->type = type_bool();
      return true;
    case TOKEN_FALSE:
      result->kind = ARG_LIT_INT;
      result->num_int = 0;
      result->type = type_bool();
      return true;
    default: UNREACHABLE("");
    }
  } break;
  case EXPR_INVOKE: {
    if (!expr_to_ir(expr, sp, ctx)) return false;

    Op *op = &da_last(&ctx->fn->fn_body);
    assert(op->kind == OP_INVOKE);

    op->invoke.ret_ignore = false;
    op->invoke.ret = (Arg) {
      .kind = ARG_VAR,
      .type = {.kind = TYPE_UNKNOWN},
      .var = alloc_var(&ctx->fn->vars),
    };
    *result = op->invoke.ret;
  } break;
  case EXPR_BINOP: {
    if (!expr_to_ir(expr, sp, ctx)) return false;

    Op *op = &da_last(&ctx->fn->fn_body);
    assert(op->kind == OP_BINOP);

    *result = op->binop.dst;
  } break;
  default: UNREACHABLE("");
  }
  return true;
}

static bool expr_to_ir(Expr *expr, Scoop *sp, Gen_Context *ctx)
{
  Op op = { .loc = expr->loc };
  static_assert(__expr_kind_count == 3, "introduced more expr kinds");
  switch (expr->kind) {
  case EXPR_INVOKE: {
    if (!expr_to_arg(expr->invoke.fn, sp, ctx, &op.invoke.fn))
      return false;

    da_foreach(Expr, expr_arg, &expr->invoke.args) {
      Arg arg = {0};
      if (!expr_to_arg(expr_arg, sp, ctx, &arg)) return false;
      da_append(&op.invoke.args, arg);
    }

    op.invoke.ret_ignore = true;
    da_append(&ctx->fn->fn_body, op);
  } break;
  case EXPR_BINOP: {
    op.kind = OP_BINOP;
    op.binop.kind = expr->binop.kind;

    if (!expr_to_arg(expr->binop.lhs, sp, ctx, &op.binop.lhs))
      return false;
    if (!expr_to_arg(expr->binop.rhs, sp, ctx, &op.binop.rhs))
      return false;

    op.binop.dst = (Arg) {
      .kind = ARG_VAR,
      .type = {.kind = TYPE_UNKNOWN},
      .var = alloc_var(&ctx->fn->vars),
    };

    da_append(&ctx->fn->fn_body, op);
  } break;
  case EXPR_ATOM: break; // this doesn't need to generate an ir op currently.
  default: UNREACHABLE("");
  }
  return true;
}

static size_t append_op(OpList *ops, Op op)
{
  da_append(ops, op);
  return ops->count - 1;
}

static size_t append_op_label(OpList *ops)
{
  size_t label = append_op(ops, (Op) {.kind = OP_LABEL});
  ops->items[label].label = label;
  return label;
}

static bool stat_to_ir(Stat *stat, Scoop *sp, Gen_Context *ctx)
{
  Op op = { .loc = stat->loc };
  static_assert(__stat_kind_count == 7, "introduced more stat kinds");
  switch (stat->kind) {
  case STAT_INVOKE: {
    op.kind = OP_INVOKE;
    if (!expr_to_arg(stat->invoke.fn, sp, ctx, &op.invoke.fn))
      return false;

    da_foreach(Expr, stat_arg, &stat->invoke.args) {
      Arg arg = {0};
      if (!expr_to_arg(stat_arg, sp, ctx, &arg)) return false;
      da_append(&op.invoke.args, arg);
    }

    op.invoke.ret_ignore = true;
    da_append(&ctx->fn->fn_body, op);
  } break;
  case STAT_RET: {
    op.kind = OP_RETURN;
    if (!expr_to_arg(stat->ret_val, sp, ctx, &op.ret_val))
      return false;
    da_append(&ctx->fn->fn_body, op);
  } break;
  case STAT_BLOCK: {
    Scoop *new_sp = new_scoop(sp);
    da_append(&ctx->sps, sp);
    da_foreach(Stat, s, &stat->block) {
      if (!stat_to_ir(s, new_sp, ctx)) return false;
    }
  } break;
  case STAT_IF: {
    Arg cond = {0};
    if (!expr_to_arg(stat->if_else.cond, sp, ctx, &cond))
      return false;

    size_t jmp_else = append_op(&ctx->fn->fn_body, (Op) {
        .kind = OP_JMP_ELSE,
        .jmp = {.cond = cond},
      });
    if (!stat_to_ir(stat->if_else.on_true, sp, ctx)) return false;

    if (stat->if_else.on_false == NULL) {
      size_t end_label = append_op_label(&ctx->fn->fn_body);
      ctx->fn->fn_body.items[jmp_else].jmp.label = end_label;
    } else {
      size_t jmp_end = append_op(&ctx->fn->fn_body, (Op){
          .kind = OP_JMP,
        });

      size_t else_label = append_op_label(&ctx->fn->fn_body);
      ctx->fn->fn_body.items[jmp_else].jmp.label = else_label;

      if (!stat_to_ir(stat->if_else.on_false, sp, ctx)) return false;

      size_t end_label = append_op_label(&ctx->fn->fn_body);
      ctx->fn->fn_body.items[jmp_end].jmp.label = end_label;
    }
  } break;
  case STAT_ASSIGN: {
    op.kind = OP_SET_VAR;
    if (!expr_to_arg(stat->assign.dst, sp, ctx, &op.set_var.var))
      return false;
    if (!expr_to_arg(stat->assign.val, sp, ctx, &op.set_var.val))
      return false;
    da_append(&ctx->fn->fn_body, op);
  } break;
  case STAT_DEF: {
    static_assert(__def_kind_count == 3,
                  "introduced more def kinds");
    switch (stat->def.kind) {
    case DEF_EXT: {
      Symbol *sym = insert_sym(sp, stat->def.name, stat->loc, SYMBOL_EXTERN);
      if (sym == NULL) return false;

      sym->ext = calloc(1, sizeof(*sym->ext));
      sym->ext->linkname = stat->def.ext.linkname;
      sym->ext->type = type_clone(stat->def.ext.type);
      da_append(&ctx->prog->externs, sym->ext);
    } break;
    case DEF_FN: {
      Symbol *sym = insert_sym(sp, stat->def.name, stat->loc, SYMBOL_FN);
      if (sym == NULL) return false;

      String_Builder name = {0};
      if (ctx->fn == NULL) {
        sb_appendf(&name, SV_Fmt, SV_Arg(stat->def.name));
      } else {
        sb_appendf(&name, ".fn_%ld", ctx->known.count);
      }
      sym->fn = push_fn_ast(ctx, name, &stat->def.fn, sp);
    } break;
    case DEF_VAR: {
      if (ctx->fn == NULL) TODO("implement global variables");
      if (stat->def.var.init != NULL) {
        op.kind = OP_SET_VAR;
        if (!expr_to_arg(stat->def.var.init, sp, ctx, &op.set_var.val))
          return false;
      }

      Symbol *sym = insert_sym(sp, stat->def.name, stat->loc, SYMBOL_VAR);
      if (sym == NULL) return false;

      sym->var = calloc(1, sizeof(*sym->var));
      da_append(&ctx->fn->vars, sym->var);

      *sym->var = (Var) {
        .type = type_clone(stat->def.var.type),
      };
      if (stat->def.var.init != NULL) {
        if (!id_to_arg(stat->def.name, stat->def.loc, &op.set_var.var, sp, ctx))
          return false;
        da_append(&ctx->fn->fn_body, op);
      }
    } break;
    default: UNREACHABLE("");
    }
  } break;
  case STAT_EMPTY:
    // nothing to do
    break;
  default: UNREACHABLE("");
  }
  return true;
}

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

bool compile_type_expr(Lexer *l, TypeExpr *type)
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

static bool compile_type_fn(Lexer *l, TypeExpr *type)
{
  assert(l->current.kind == TOKEN_FN);

  bool result;

  type->kind = TYPE_FN;
  if (!prefetch_expect_token(l, '(')) return_defer(false);

  if (!prefetch_not_none(l)) return_defer(false);
  type->fn_type.arg_types = (TypeList) {0};
  while (l->current.kind != ')') {
    if (l->current.kind == TOKEN_DOTS) { // parse "..." for va_args
      if (!prefetch_expect_token(l, ')')) return false;
      type->fn_type.va_args = true;
    } else {
      // failures in this loop would not cause memory leak;
      TypeExpr arg_type = {0};
      // memory would clean up in the compile_type_expr;
      if (!compile_type_expr(l, &arg_type)) return_defer(false);
      da_append(&type->fn_type.arg_types, arg_type);
      // from here, the ownership has moved to ext.type.arg_types;
      // thus, they will be clean up together with ext.type.arg_types;
      if (!prefetch_expect_tokens(l, ',', ')')) return_defer(false);
      if (l->current.kind == ',') {
        if (!prefetch_not_none(l)) return_defer(false);
      }
    }
  }

  if (!prefetch_expect_token(l, ':')) return_defer(false);
  if (!prefetch_not_none(l)) return_defer(false);

  type->fn_type.ret_type = calloc(1, sizeof(TypeExpr));
  assert(type->fn_type.ret_type);
  if (!compile_type_expr(l, type->fn_type.ret_type)) return_defer(false);

  return_defer(true);

 defer:
  if (!result) destroy_type_expr(type);
  return result;
}

static bool detect_arg_type(Arg *arg) {
  if (arg->type.kind != TYPE_UNKNOWN) return true;
  if (arg->kind == ARG_NONE) return true;

  TypeExpr *type = NULL;

  static_assert(__arg_kind_count == 6, "introduced more arg kinds");
  switch(arg->kind) {
  case ARG_EXTERN:
    type = &arg->ext->type;
    break;
  case ARG_FN:
    type = &arg->fn->type;
    break;
  case ARG_VAR:
    type = &arg->var->type;
    break;
  default:
    UNREACHABLE("detect_arg_type");
  }

  assert(type != NULL);
  assert(type->kind != TYPE_UNKNOWN);
  arg->type = type_clone(*type);
  return true;
}

static bool detect_var_type(Arg *var, Arg *val)
{
  assert(val->type.kind != TYPE_UNKNOWN);
  if (var->type.kind != TYPE_UNKNOWN) return true;

  TypeExpr *var_type = NULL;
  // not every arg type can occur in here
  static_assert(__arg_kind_count == 6, "introduced more arg kinds");
  switch (var->kind) {
  case ARG_VAR:
    var_type = &var->var->type;
    break;
  default: UNREACHABLE("fix_type_var");
  }

  if (var_type->kind == TYPE_UNKNOWN) {
    *var_type = type_clone(val->type);
  }

  var->type = type_clone(val->type);
  return true;
}

static bool detect_binop_dst_type(OpBinop *binop)
{
  Arg *lhs = &binop->lhs;
  Arg *rhs = &binop->rhs;
  Arg *dst = &binop->dst;

  assert(lhs->type.kind != TYPE_UNKNOWN);
  assert(rhs->type.kind != TYPE_UNKNOWN);
  assert(dst->kind == ARG_VAR);

  static_assert(__binop_kind_count == 11, "introduced more binop kinds");
  switch (binop->kind) {
  case BINOP_EQ:
  case BINOP_NEQ:
  case BINOP_LS:
  case BINOP_GT:
  case BINOP_LE:
  case BINOP_GE:
    if (!type_eq(&lhs->type, &rhs->type)) {
      pcompile_info(lhs->loc,
                    "error: lhs and rhs of operator `%s` "
                    "is not same.\n",
                    binop_name(binop->kind));
      pcompile_info(lhs->loc, "info: lhs is in type ");
      dump_type_expr(&lhs->type, stderr);
      fprintf(stderr, "\n");

      pcompile_info(lhs->loc, "info: rhs is in type ");
      dump_type_expr(&lhs->type, stderr);
      fprintf(stderr, "\n");
      return false;
    }

    dst->var->type = type_bool();

    break;
  case BINOP_SUB:
  case BINOP_ADD:
  case BINOP_MUL:
  case BINOP_DIV:
  case BINOP_MOD:
    if (lhs->type.kind != TYPE_INT && lhs->type.kind != TYPE_UINT) {
      pcompile_info(lhs->loc,
                    "error: lhs of operator `%s` "
                    "is expected to be a integer, but bot a ",
                    binop_name(binop->kind));
      dump_type_expr(&lhs->type, stderr);
      fprintf(stderr, "\n");
      return false;
    }

    if (rhs->type.kind != TYPE_INT && rhs->type.kind != TYPE_UINT) {
      pcompile_info(rhs->loc,
                    "error: rhs of operator `%s` "
                    "is expected to be a integer, but bot a ",
                    binop_name(binop->kind));
      dump_type_expr(&rhs->type, stderr);
      fprintf(stderr, "\n");
      return false;
    }

    if (!type_eq(&lhs->type, &rhs->type)) {
      pcompile_info(lhs->loc,
                    "error: lhs and rhs of operator `%s` "
                    "is not same.\n",
                    binop_name(binop->kind));
      pcompile_info(lhs->loc, "info: lhs is in type ");
      dump_type_expr(&lhs->type, stderr);
      fprintf(stderr, "\n");

      pcompile_info(lhs->loc, "info: rhs is in type ");
      dump_type_expr(&lhs->type, stderr);
      fprintf(stderr, "\n");
      return false;
    }

    dst->type = type_clone(lhs->type);
    dst->var->type = type_clone(lhs->type);

    return true;
    break;
  default:
    UNREACHABLE("");
  }

  return true;
}

static bool detect_all_unknown_type(Program *prog)
{
  da_foreach (Fn*, fn_ptr, &prog->fn_list) {
    Fn *fn = *fn_ptr;
    da_foreach (Op, op, &fn->fn_body) {
      static_assert(__op_kind_count == 7, "introduced more op kinds");
      switch (op->kind) {
      case OP_RETURN:
        if (!detect_arg_type(&op->ret_val)) return false;
        break;
      case OP_SET_VAR:
        if (!detect_arg_type(&op->set_var.val)) return false;
        if (!detect_var_type(&op->set_var.var, &op->set_var.val)) return false;
        break;
      case OP_INVOKE: {
        if (!detect_arg_type(&op->invoke.fn)) return false;
        da_foreach (Arg, arg, &op->invoke.args) {
          if (!detect_arg_type(arg)) return false;
        }

        if (!op->invoke.ret_ignore) {
          assert(op->invoke.ret.kind == ARG_VAR);
          Var *ret = op->invoke.ret.var;
          TypeExpr *ret_type = fn->type.fn_type.ret_type;

          assert(ret_type->kind != TYPE_UNKNOWN);
          ret->type = type_clone(*ret_type);
        }
      } break;
      case OP_BINOP:
        if (!detect_arg_type(&op->binop.lhs)) return false;
        if (!detect_arg_type(&op->binop.rhs)) return false;
        if (!detect_binop_dst_type(&op->binop)) return false;
        break;
      case OP_JMP_ELSE:
        if (!detect_arg_type(&op->jmp.cond)) return false;
        break;
      case OP_JMP:
      case OP_LABEL:
        // nothing to do because these op have no arguments.
        break;
      default: UNREACHABLE("op");
      }
    }
  }
  return true;
}

static bool check_type(Program *prog)
{
  da_foreach (Fn *, fn_ptr, &prog->fn_list) {
    Fn *fn = *fn_ptr;
    TypeExpr *fn_type = &fn->type;

    assert(fn_type->kind == TYPE_FN);
    da_foreach (Op, op, &fn->fn_body) {
      static_assert(__op_kind_count == 7, "introduced more op kinds");
      switch (op->kind) {
      case OP_RETURN: {
        TypeExpr *ret_type = &op->ret_val.type;
        if (!type_matched(fn_type->fn_type.ret_type, ret_type)) {
          pcompile_info(op->loc,
                        "error: the return type of this function "
                        "is required to be ");
          dump_type_expr(fn_type->fn_type.ret_type, stderr);
          fprintf(stderr, ", but got ");
          dump_type_expr(ret_type, stderr);
          fputc('\n', stderr);
          return false;
        }
      } break;
      case OP_SET_VAR:
        if (!type_matched(&op->set_var.var.type, &op->set_var.val.type)) {
          pcompile_info(op->loc,
                        "error: incompatible types when assigning to type \"");
          dump_type_expr(&op->set_var.var.type, stderr);
          fprintf(stderr, "\" from type \"");
          dump_type_expr(&op->set_var.val.type, stderr);
          fprintf(stderr, "\"\n");
          return false;
        }

        break;
      case OP_INVOKE: {
        // TODO: report a better error message.
        if (op->invoke.fn.type.kind != TYPE_FN) {
          pcompile_info(op->loc, "error: try to invoke an uncallable value\n");
          return false;
        }
        TypeExpr *invoked_type = &op->invoke.fn.type;
        TypeList *expected_types = &invoked_type->fn_type.arg_types;

        bool size_matched = invoked_type->fn_type.va_args?
          expected_types->count <= op->invoke.args.count:
          expected_types->count == op->invoke.args.count;
        if (!size_matched) {
          pcompile_info(op->loc,
                        "error: this function expected %ld arguments, but got %ld arguments\n",
                        expected_types->count, op->invoke.args.count);
          return false;
        }

        assert(expected_types->count <= op->invoke.args.count);
        for (size_t i = 0; i < expected_types->count; ++i) {
          TypeExpr *expected = &expected_types->items[i];
          TypeExpr *actual = &op->invoke.args.items[i].type;
          if (!type_matched(expected, actual)) {
            pcompile_info(op->loc, "error: the %ld-th argument is expected to be ", i);
            dump_type_expr(expected, stderr);
            fprintf(stderr, ", but got ");
            dump_type_expr(actual, stderr);
            fputc('\n', stderr);
            return false;
          }
        }
      } break;
      case OP_JMP_ELSE: {
        TypeExpr *cond_type = &op->jmp.cond.type;
        if (cond_type->kind != TYPE_BOOL) {
          pcompile_info(op->loc,
                        "error: the condition of 'if' statement "
                        "is required to be 'bool'");
          fprintf(stderr, ", but got ");
          dump_type_expr(cond_type, stderr);
          fputc('\n', stderr);
          return false;
        }
      } break;
      case OP_BINOP:
        // the type of dst is detected in detect_all_unknown_type
        // and if it could be detected successfully, it must be available
        // thus, it doesn't need additional checks.
      case OP_LABEL:
      case OP_JMP:
        // These op has no args
        break;
      default: UNREACHABLE("op");
      }
    }
  }
  return true;
}

static bool check_fn_returned(Program *prog)
{
  da_foreach(Fn *, fn_ptr, &prog->fn_list) {
    Fn *fn = *fn_ptr;
    bool returned = false;
    da_foreach(Op, op, &fn->fn_body) {
      static_assert(__op_kind_count == 7, "introduced more op kinds");
      switch(op->kind) {
      case OP_RETURN:
        returned = true;
        break;
      default: break;
      }
    }

    if (!returned) {
      assert(fn->type.kind == TYPE_FN);
      if (fn->type.fn_type.ret_type->kind != TYPE_VOID) {
        pcompile_info(fn->loc, "error: this function is never returned\n");
        return false;
      } else {
        Op op = {
          .kind = OP_RETURN,
        };
        da_append(&fn->fn_body, op);
      }
    }
  }
  return true;
}

static bool gen_ir_fn(Fn *fn, Gen_Context *ctx)
{
  ctx->fn = fn;

  Fn_Ctx *fn_ctx = ht_find(&ctx->known, fn);
  assert(fn_ctx != NULL);
  da_append(&ctx->prog->fn_list, fn);
  AST_Fn *ast = fn_ctx->fn;

  fn->loc = ast->loc;
  fn->type.kind = TYPE_FN;
  da_foreach(Def, arg, &ast->args) {
    assert(arg->kind == DEF_VAR);
    if (arg->var.init != NULL) TODO("support default argument for function");
    Symbol *sym = insert_sym(fn_ctx->sp, arg->name, arg->loc, SYMBOL_VAR);
    if (sym == NULL) return false;
    sym->var = calloc(1, sizeof(*sym->var));
    sym->var->type = type_clone(arg->var.type);

    da_append(&ctx->fn->vars, sym->var);
    da_append(&fn->type.fn_type.arg_types, type_clone(arg->var.type));
  }
  fn->type.fn_type.ret_type = malloc(sizeof(TypeExpr));
  *fn->type.fn_type.ret_type = type_clone(ast->ret_type);

  da_foreach (Stat, stat, &ast->body) {
    if (!stat_to_ir(stat, fn_ctx->sp, ctx)) return false;
  }
  //  if (!stat_to_ir(ast->body, NULL, ctx)) return false;
  for (size_t i = 0; i < fn->vars.count; ++i) {
    fn->vars.items[i]->id = i;
  }
  return true;
}

static bool gen_ir(Program *prog, Stat_List *stats)
{
  bool result = true;

  Gen_Context ctx = {.prog = prog};
  Scoop *global = new_scoop(NULL);
  da_append(&ctx.sps, global);
  da_foreach(Stat, stat, stats) {
    if (stat->kind != STAT_DEF) {
      pcompile_info(stat->loc, "error: only definations are available here.\n");
      result = false;
      continue;
    }
    if (!stat_to_ir(stat, global, &ctx)) result = false;
  }

  FnList *ungenerated = &ctx.ungenerated;
  while (ungenerated->count != 0) {
    Fn *fn = da_pop(ungenerated);
    if (!gen_ir_fn(fn, &ctx)) result = false;
  }

  da_free(ctx.ungenerated);
  ht_free(&ctx.known);
  da_foreach(Scoop *, sp, &ctx.sps) {
    ht_free(&(*sp)->symbols);
    free(*sp);
  }
  da_free(ctx.sps);

  return result;
}

bool compile_program(Lexer *l, Program *prog)
{
  Stat_List stats = {0};
  if (!compile_file(l, &stats)) {
    stat_list_del(stats);
    return false;
  }
  if (!gen_ir(prog, &stats)) {
    stat_list_del(stats);
    return false;
  }

  stat_list_del(stats);

  if (!detect_all_unknown_type(prog)) return false;
  if (!check_type(prog)) return false;
  if (!check_fn_returned(prog)) return false;

  return true;
}

void destroy_program(Program *prog)
{
  da_foreach (Extern *, ext_ptr, &prog->externs) {
    Extern *ext = *ext_ptr;
    destroy_type_expr(&ext->type);
    free(ext);
  }
  da_free(prog->externs);

  destroy_fn_list(&prog->fn_list);
  if (prog->str_lits.capacity > 0) {
    da_free(prog->str_lits);
  }
}

