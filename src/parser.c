#include "parser.h"

#include "3rd_wrapper.h"

#include "lexer.h"
#include "type.h"
#include "ast.h"

typedef struct Scope Scope;
struct Scope {
  Scope *upper;
  Ht(String_View, Arg) values;
};

static Scope *new_scope(Scope *upper)
{
  Scope *s = arena_calloc(1, sizeof(*s));
  s->values.hasheq = ht_sv_hasheq;
  s->upper = upper;
  return s;
}

static Arg *scope_add(Scope *sp, String_View name, Cursor loc)
{
  Arg *value = ht_find(&sp->values, name);
  if (value != NULL) {
    pcompile_info(loc,
                  "error: symbol "SV_Fmt" redefined in this scope\n",
                  SV_Arg(name));
    // TODO: report where the symbol is first defined;
    return NULL;
  } else {
    value = ht_put(&sp->values, name);
    value->loc = loc;
    return value;
  }
}

// C is so bad
typedef struct {
  Scope *scope;
  Arg   *value;
} SymSearchResult;

static SymSearchResult sym_search(Scope *sp, String_View name)
{
  for (Scope *s = sp; s != NULL; s = s->upper) {
    Arg *value = ht_find(&s->values, name);
    if (value != NULL) {
      return (SymSearchResult) {
        .scope = s,
        .value = value,
      };
    }
  }

  return (SymSearchResult) {NULL, NULL};
}

static Var *alloc_var(VarList *vars, TypeExpr type)
{
  Var *var = arena_alloc(sizeof(Var));
  *var = (Var) {
    .type = type_clone(type),
  };
  da_append(vars, var);
  return var;
}

static void dump_arg(String_Builder *sb, Arg *arg)
{
  static_assert(__arg_kind_count == 5, "introduced more arg kinds");
  switch(arg->kind) {
  case ARG_NONE:
    sb_appendf(sb, "None");
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
  Lambda *fn;
  Scope *sp;
} Fn_Ctx;

typedef Ht(Fn*, Fn_Ctx) Known_Fn;

typedef struct {
  Program *prog;
  Fn *fn;

  FnList ungenerated;
  Known_Fn known;
} Gen_Context;

static Fn *push_fn(Lambda* lambda, Gen_Context *ctx, Scope *sp)
{
  Fn *fn = arena_calloc(1, sizeof(*fn));
  da_append(&ctx->ungenerated, fn);

  // default name of a function is defined here
  // and it may be override in stat_to_ir
  sb_appendf(&fn->name, ".lambda_%ld", ctx->known.count);
  fn->type = type_of_fn(&lambda->ret_type, &lambda->args);

  fn->is_extern = lambda->is_extern;
  *ht_put(&ctx->known, fn) = (Fn_Ctx) {
    .fn = lambda,
    .sp = new_scope(sp),
  };
  return fn;
}

static bool id_to_arg(String_View name, Cursor loc, Arg *arg, Scope *sp, Gen_Context *ctx)
{
  SymSearchResult r = sym_search(sp, name);
  if (r.scope == NULL) {
    pcompile_info(loc,
                  "error: cannot find `"SV_Fmt"` in this scope\n",
                  SV_Arg(name));
    return NULL;
  }

  if (r.value->kind == ARG_VAR) {
    if (!contains(ctx->fn->vars.items, ctx->fn->vars.count, r.value->var)) {
      // TODO: support global variables
      pcompile_info(loc,
                    "error: try to visit a nonlocal variable\n");
      pcompile_info(r.value->loc,
                    "info: `"SV_Fmt"` is defined in here\n",
                    SV_Arg(name));
      return NULL;
    }
  }

  *arg     = *r.value;
  arg->loc = loc;

  return arg;
}

static bool invoke_available(Cursor loc, AST_Invoke *invoke, Scope *sp);

static bool detect_binop_type(Expr *expr, Scope *sp, TypeExpr expected);

static bool detect_expr_type(Expr *expr, Scope *sp, TypeExpr expected)
{
  if (expr->type.kind == TYPE_UNKNOWN) {
    static_assert(__expr_kind_count == 6, "introduced more expr kinds");
    switch (expr->kind) {
    case EXPR_BINOP:
      return detect_binop_type(expr, sp, expected);
      break;
    case EXPR_NAME: {
      SymSearchResult r = sym_search(sp, expr->name);
      if (r.scope == NULL) {
        pcompile_info(expr->loc,
                      "error: cannot find `"SV_Fmt"` in this scope\n",
                      SV_Arg(expr->name));
        return NULL;
      }
      expr->type = type_clone(r.value->type);
    } break;
    case EXPR_INVOKE:
      if (!invoke_available(expr->loc, &expr->invoke, sp)) return false;
      expr->type = type_clone(*expr->invoke.fn->type.fn_type.ret_type);
      break;
    case EXPR_INT:
    case EXPR_STR:
    case EXPR_LAMBDA:
      assert(false && "these type must be known, it may be a bug in ast.h");
      break;
    default: UNREACHABLE("");
    }
  }

  if (expected.kind == TYPE_INT || expected.kind == TYPE_UINT) {
    if (expr->kind == EXPR_INT && expr->type.kind != TYPE_BOOL) {
      expr->type = expected;
    }
  }

  if (expected.kind != TYPE_UNKNOWN && !type_matched(&expected, &expr->type)) {
    pcompile_info(expr->loc, "error: here expected a ");
    dump_type_expr(&expected, stderr);
    fprintf(stderr, ", but got ");
    dump_type_expr(&expr->type, stderr);
    fputc('\n', stderr);
    return false;
  } else {
    return true;
  }
}

static bool detect_binop_type(Expr *expr, Scope *sp, TypeExpr expected)
{
  assert(expr->kind == EXPR_BINOP && expr->type.kind == TYPE_UNKNOWN);
  bool ok = true;

  Expr *lhs = expr->binop.lhs;
  Expr *rhs = expr->binop.rhs;

  static_assert(__binop_kind_count == 11, "introduced more binop kinds");
  switch(expr->binop.kind) {
  case BINOP_ADD:
  case BINOP_SUB:
  case BINOP_MUL:
  case BINOP_DIV:
  case BINOP_MOD: {
    if (!detect_expr_type(lhs, sp, expected)) ok = false;
    if (!detect_expr_type(rhs, sp, expected)) ok = false;
    size_t maxsize = lhs->type.size > rhs->type.size?
      lhs->type.size : rhs->type.size;
    bool sign = lhs->type.kind == TYPE_INT || rhs->type.kind == TYPE_INT;
    expr->type = type_int(sign, maxsize);
  } break;
  case BINOP_EQ:
  case BINOP_NEQ:
  case BINOP_LS:
  case BINOP_GT:
  case BINOP_LE:
  case BINOP_GE: {
    if (!detect_expr_type(lhs, sp, type_unknown())) ok = false;
    if (!detect_expr_type(rhs, sp, type_unknown())) ok = false;

    if (lhs->type.kind != TYPE_INT && lhs->type.kind != TYPE_UINT) {
      pcompile_info(lhs->loc, "error: only numbers are comparable, but got a ");
      dump_type_expr(&lhs->type, stderr);
      fputc('\n', stderr);
      ok = false;
    }

    if (rhs->type.kind != TYPE_INT && rhs->type.kind != TYPE_UINT) {
      pcompile_info(rhs->loc, "error: only numbers are comparable, but got a ");
      dump_type_expr(&rhs->type, stderr);
      fputc('\n', stderr);
      ok = false;
    }
    expr->type = type_bool();
  } break;
  default: UNREACHABLE("");
  }
  return ok;
}

static bool invoke_available(Cursor loc, AST_Invoke *invoke, Scope *sp)
{
  if (!detect_expr_type(invoke->fn, sp, type_unknown())) return false;
  if (invoke->fn->type.kind != TYPE_FN) {
    pcompile_info(loc, "error: here expected a function but got ");
    dump_type_expr(&invoke->fn->type, stderr);
    fputc('\n', stderr);
  }

  FnType fn_type = invoke->fn->type.fn_type;
  bool arg_matched = true;
  if (fn_type.arg_types.count > invoke->args.count)
    arg_matched = false;
  if (!fn_type.va_args && fn_type.arg_types.count < invoke->args.count)
    arg_matched = false;
  if (!arg_matched) {
    pcompile_info(loc,
                  "error: this function expects %ld arguments, but provided %ld.\n",
                  fn_type.arg_types.count,
                  invoke->args.count);
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < invoke->args.count; ++ i) {
    TypeExpr expect = type_unknown();
    if (i < fn_type.arg_types.count)
      expect = fn_type.arg_types.items[i];

    if (!detect_expr_type(&invoke->args.items[i], sp, expect)) ok = false;
  }
  return ok;
}

static bool expr_to_ir(Expr *expr, Scope *sp, Gen_Context *ctx);

static bool expr_to_arg(Expr *expr, Scope *sp, Gen_Context *ctx, Arg *result)
{
  result->loc  = expr->loc;
  result->type = expr->type;

  static_assert(__expr_kind_count == 6, "introduced more expr kinds");
  switch (expr->kind) {
  case EXPR_NAME:
    return id_to_arg(expr->name, expr->loc, result, sp, ctx);
  case EXPR_STR:
    result->kind      = ARG_LIT_STR;
    result->str_label = compile_strlit(ctx->prog, expr->str);
    return true;
  case EXPR_INT:
    result->kind      = ARG_LIT_INT;
    result->num_int   = expr->integer;
    return true;
  case EXPR_INVOKE: {
    if (!expr_to_ir(expr, sp, ctx)) return false;

    Op *op = &da_last(&ctx->fn->fn_body);
    assert(op->kind == OP_INVOKE);

    op->invoke.ret_ignore = false;
    op->invoke.ret = (Arg) {
      .kind = ARG_VAR,
      .type = type_clone(expr->type),
      .var = alloc_var(&ctx->fn->vars, expr->type),
    };
    *result = op->invoke.ret;
    return true;
  } case EXPR_BINOP: {
    if (!expr_to_ir(expr, sp, ctx)) return false;

    Op *op = &da_last(&ctx->fn->fn_body);
    assert(op->kind == OP_BINOP);

    *result = op->binop.dst;
    return true;
  } case EXPR_LAMBDA: {
    result->kind = ARG_FN;
    result->fn   = push_fn(&expr->lambda, ctx, sp);
    return true;
  } default: UNREACHABLE("");
  }
  return true;
}

static bool expr_to_ir(Expr *expr, Scope *sp, Gen_Context *ctx)
{
  Op op = { .loc = expr->loc };
  static_assert(__expr_kind_count == 6, "introduced more expr kinds");
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
      .type = type_clone(expr->type),
      .var = alloc_var(&ctx->fn->vars, expr->type),
    };

    da_append(&ctx->fn->fn_body, op);
  } break;
  case EXPR_LAMBDA:
  case EXPR_STR:
  case EXPR_NAME:
  case EXPR_INT:
    break; // this doesn't need to generate an ir op currently.
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

static bool compiletime_eval_algebra(Arg lhs, Arg rhs, BinopKind op, Arg *val)
{
  val->kind = ARG_LIT_INT;
  static_assert(__binop_kind_count == 11, "introduced more binop kinds");
  switch (op) {
  case BINOP_ADD:
    val->num_int = lhs.num_int + rhs.num_int;
    break;
  case BINOP_SUB:
    val->num_int = lhs.num_int - rhs.num_int;
    break;
  case BINOP_MUL:
    val->num_int = lhs.num_int * rhs.num_int;
    break;
  case BINOP_DIV:
    val->num_int = lhs.num_int / rhs.num_int;
    break;
  case BINOP_MOD:
    val->num_int = lhs.num_int % rhs.num_int;
    break;
  default: UNREACHABLE("compiletime_eval_binop");
  }
  return true;
}

static bool compiletime_eval_cmp(Arg lhs, Arg rhs, BinopKind op, Arg *val)
{
  val->kind = ARG_LIT_INT;
  static_assert(__binop_kind_count == 11, "introduced more binop kinds");
  switch (op) {
  case BINOP_EQ:
    val->num_int = lhs.num_int == rhs.num_int ? 1 : 0;
    break;
  case BINOP_NEQ:
    val->num_int = lhs.num_int != rhs.num_int ? 1 : 0;
    break;
  case BINOP_LS:
    val->num_int = lhs.num_int <  rhs.num_int ? 1 : 0;
    break;
  case BINOP_GT:
    val->num_int = lhs.num_int >  rhs.num_int ? 1 : 0;
    break;
  case BINOP_LE:
    val->num_int = lhs.num_int <= rhs.num_int ? 1 : 0;
    break;
  case BINOP_GE:
    val->num_int = lhs.num_int >= rhs.num_int ? 1 : 0;
    break;
  default: UNREACHABLE("compiletime_eval_binop");
  }
  return true;
}

static bool expr_eval(Expr* expr, Scope *sp, Gen_Context *ctx, Arg *val)
{
  val->type = type_clone(expr->type);
  static_assert(__expr_kind_count == 6, "introduced more expr kinds");
  switch (expr->kind) {
  case EXPR_INVOKE:
    pcompile_info(expr->loc,
                  "error: invoking a function is not allowed in compile time\n");
    return false;
  case EXPR_INT:
  case EXPR_STR:
  case EXPR_LAMBDA:
    return expr_to_arg(expr, sp, ctx, val);
  case EXPR_NAME:
    if (!expr_to_arg(expr, sp, ctx, val)) return false;
    if (val->kind == ARG_VAR) {
      pcompile_info(val->loc,
                    "error: `"SV_Fmt"` is a runtime variable, which value is unkown at compiletime.\n",
                    SV_Arg(expr->name));
      return false;
    }
    return true;
  case EXPR_BINOP: {
    Arg lhs = {0};
    if (!expr_eval(expr->binop.lhs, sp, ctx, &lhs)) return false;
    Arg rhs = {0};
    if (!expr_eval(expr->binop.rhs, sp, ctx, &rhs)) return false;

    static_assert(__binop_kind_count == 11, "introduced more binop kinds");
    switch (expr->binop.kind) {
    case BINOP_ADD:
    case BINOP_SUB:
    case BINOP_MUL:
    case BINOP_DIV:
    case BINOP_MOD:
      return compiletime_eval_algebra(lhs, rhs, expr->binop.kind, val);
      break;
    case BINOP_EQ:
    case BINOP_NEQ:
    case BINOP_LS:
    case BINOP_GT:
    case BINOP_LE:
    case BINOP_GE:
      return compiletime_eval_cmp(lhs, rhs, expr->binop.kind, val);
      break;
    default: UNREACHABLE("");
    }
    return true;
  } default: UNREACHABLE("");
  }
}

static bool stat_to_ir(Stat *stat, Scope *sp, Gen_Context *ctx)
{
  Op op = { .loc = stat->loc };
  static_assert(__stat_kind_count == 7, "introduced more stat kinds");
  switch (stat->kind) {
  case STAT_INVOKE: {
    op.kind = OP_INVOKE;

    if (!invoke_available(stat->loc, &stat->invoke, sp)) return false;
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
    if (!detect_expr_type(stat->ret_val, sp, *ctx->fn->type.fn_type.ret_type))
      return false;
    if (!expr_to_arg(stat->ret_val, sp, ctx, &op.ret_val))
      return false;
    da_append(&ctx->fn->fn_body, op);
  } break;
  case STAT_BLOCK: {
    Scope *new_sp = new_scope(sp);
    da_foreach(Stat, s, &stat->block) {
      if (!stat_to_ir(s, new_sp, ctx)) return false;
    }
  } break;
  case STAT_IF: {
    Arg cond = {0};
    if (!detect_expr_type(stat->if_else.cond, sp, type_bool())) return false;
    if (!expr_to_arg(stat->if_else.cond, sp, ctx, &cond))
      return false;

    size_t jmp_else = append_op(&ctx->fn->fn_body, (Op) {
        .loc = stat->if_else.cond->loc,
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
    if (!detect_expr_type(stat->assign.dst, sp, type_unknown())) return false;
    if (!expr_to_arg(stat->assign.dst, sp, ctx, &op.set_var.var))
      return false;

    TypeExpr expected = stat->assign.dst->type;
    assert(expected.kind != TYPE_UNKNOWN &&
           "the type of the destination in assignment must be known, "
           "this may be a bug in stat_to_ir ot detect_expr_type.");

    if (!detect_expr_type(stat->assign.val, sp, expected)) return false;
    if (!expr_to_arg(stat->assign.val, sp, ctx, &op.set_var.val))
      return false;
    da_append(&ctx->fn->fn_body, op);
  } break;
  case STAT_DEF: {
    if (stat->def.type.kind == TYPE_UNKNOWN) {
      if (stat->def.val == NULL) {
        pcompile_info(stat->loc,
                      "error: the type of `"SV_Fmt"` is not provided\n",
                      SV_Arg(stat->def.name));
        return false;
      }
      if (!detect_expr_type(stat->def.val, sp, type_unknown())) return false;
      stat->def.type = type_clone(stat->def.val->type);
    } else if (stat->def.val != NULL) {
      if (!detect_expr_type(stat->def.val, sp, stat->def.type)) return false;
    }
    static_assert(__def_kind_count == 2,
                  "introduced more def kinds");
    switch (stat->def.kind) {
    case DEF_LET: {
      assert(stat->def.val != NULL);

      Arg val = {0};
      if (!expr_eval(stat->def.val, sp, ctx, &val)) return false;

      Arg *arg = scope_add(sp, stat->def.name, stat->loc);
      if (arg == NULL) return false;
      *arg = val;

      // only global `let` bindings and external function need a meaningful name.
      // TODO: make functions have multiple names.
      // consider this case:
      // ```
      // let foo = fn () -> i32 {
      //    printf("foo\n");
      // }
      // let main = foo;
      // ```
      // the `main` function is expected to be a alian of `foo`
      // but currently, `main` is just not defined in assembly level.
      // TODO: consider a better way to define a external function.
      // `let printf = fn(&i32, ...)->i32 @extern` is so weird
      // because `fn(&i32, ...)->i32 @extern` is not a valid lambda actually
      // and will never work in `car printf = fn(&i32, ...)->i32 @extern`
      // or in `(fn(&i32, ...)->i32 @extern)("hello, world\n")`.
      // `let printf: fn(&i32, ...) @extern` may be a better solusion.
      if (arg->kind == ARG_FN && (ctx->fn == NULL || arg->fn->is_extern)) {
        String_Builder name = {0};
        sb_append_sv(&name, stat->def.name);
        arg->fn->name = name;
      }
    } break;
    case DEF_VAR: {
      if (ctx->fn == NULL) TODO("implement global variables");
      if (stat->def.val != NULL) {
        op.kind = OP_SET_VAR;
        if (!expr_to_arg(stat->def.val, sp, ctx, &op.set_var.val))
          return false;
      }

      Arg *arg = scope_add(sp, stat->def.name, stat->loc);
      if (arg == NULL) return false;
      arg->kind = ARG_VAR;
      arg->var  = alloc_var(&ctx->fn->vars, stat->def.type);
      arg->type = type_clone(stat->def.type);

      if (stat->def.val != NULL) {
        if (!id_to_arg(stat->def.name, stat->loc, &op.set_var.var, sp, ctx))
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

static bool gen_ir_fn(Fn *fn, Gen_Context *ctx)
{
  ctx->fn = fn;

  if (fn->is_extern) return true;

  Fn_Ctx *fn_ctx = ht_find(&ctx->known, fn);
  assert(fn_ctx != NULL);
  da_append(&ctx->prog->fn_list, fn);
  Lambda *lambda = fn_ctx->fn;

  fn->loc = lambda->loc;
  da_foreach(Fn_Arg, arg, &lambda->args) {
    Arg *value = scope_add(fn_ctx->sp, arg->name, arg->loc);
    if (value == NULL) return false;

    value->kind = ARG_VAR;
    value->var  = alloc_var(&ctx->fn->vars, arg->type);
    value->type = type_clone(arg->type);
  }

  da_foreach (Stat, stat, &lambda->body) {
    if (!stat_to_ir(stat, fn_ctx->sp, ctx)) return false;
  }

  if (fn->fn_body.count == 0 || da_last(&fn->fn_body).kind != OP_RETURN) {
    da_append(&fn->fn_body, ((Op) {
        .kind = OP_RETURN,
        .ret_val = {.kind = ARG_NONE},
        }));
  }

  for (size_t i = 0; i < fn->vars.count; ++i) {
    fn->vars.items[i]->id = i;
  }
  return true;
}

static Program *gen_ir(Stat_List *stats)
{
  bool ok = true;
  Gen_Context ctx = {.prog = arena_alloc(sizeof(*ctx.prog))};
  Scope *global = new_scope(NULL);
  da_foreach(Stat, stat, stats) {
    if (stat->kind != STAT_DEF) {
      pcompile_info(stat->loc,
                    "error: only definations are available in global scope.\n");
      ok = false;
      continue;
    }
    if (!stat_to_ir(stat, global, &ctx)) ok = false;
  }

  while (ctx.ungenerated.count != 0) {
    Fn *fn = da_pop(&ctx.ungenerated);
    if (!gen_ir_fn(fn, &ctx)) ok = false;
  }

  return ok ? ctx.prog : NULL;
}

Program *compile_program(Lexer *l)
{
  Stat_List stats = {0};
  if (!compile_file(l, &stats)) return NULL;

  Program *prog = gen_ir(&stats);
  if (prog == NULL)                   return NULL;

  return prog;
}

