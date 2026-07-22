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

Var *alloc_var(VarList *vars)
{
  Var *var = arena_alloc(sizeof(Var));
  *var = (Var) {
    .type = {.kind = TYPE_UNKNOWN},
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

  *arg = *r.value;
  arg->loc = loc;

  return arg;
}

static bool expr_to_ir(Expr *expr, Scope *sp, Gen_Context *ctx);

static bool expr_to_arg(Expr *expr, Scope *sp, Gen_Context *ctx, Arg *result)
{
  static_assert(__expr_kind_count == 4, "introduced more expr kinds");
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
  case EXPR_LAMBDA:
    TypeList arg_types = {0};
    da_foreach(Fn_Arg, arg, &expr->lambda.args) {
      da_append(&arg_types, arg->type);
    }

    result->kind = ARG_FN;
    result->loc  = expr->loc;
    result->type = type_fn(expr->lambda.ret_type,
                           arg_types,
                           expr->lambda.args.va);
    result->fn   = push_fn(&expr->lambda, ctx, sp);
    break;
  default: UNREACHABLE("");
  }
  return true;
}

static bool expr_to_ir(Expr *expr, Scope *sp, Gen_Context *ctx)
{
  Op op = { .loc = expr->loc };
  static_assert(__expr_kind_count == 4, "introduced more expr kinds");
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
  case EXPR_LAMBDA:
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

static bool stat_to_ir(Stat *stat, Scope *sp, Gen_Context *ctx)
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
    Scope *new_sp = new_scope(sp);
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
    Arg *arg = scope_add(sp, stat->def.name, stat->loc);
    if (arg == NULL) return false;

    static_assert(__def_kind_count == 2,
                  "introduced more def kinds");
    switch (stat->def.kind) {
    case DEF_LET: {
      assert(stat->def.val != NULL);

      Expr *expr = expr_eval(stat->def.val);
      if (expr == NULL) return false;

      if (!expr_to_arg(expr, sp, ctx, arg)) return false;

      // only global `let` bindings and external function need a meaningful name
      // TODO: make functions have multiple names
      // consider this case:
      // ```
      // let foo = fn () -> i32 {
      //    printf("foo\n");
      // }
      // let main = foo;
      // ```
      // the `main` function is expected to be a alian of `foo`
      // but currently, `main` is just not defined in assebly level
      // TODO: consider a better way to define a external function
      // `let printf = fn(&i32, ...)->i32 @extern` is so weird
      // because `fn(&i32, ...)->i32 @extern` is not a valid lambda actually
      // and will never work in `car printf = fn(&i32, ...)->i32 @extern`
      // or in `(fn(&i32, ...)->i32 @extern)("hello, world\n")`
      if (expr->kind == EXPR_LAMBDA && (ctx->fn == NULL || expr->lambda.is_extern)) {
        assert(arg->kind == ARG_FN);

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

      arg->kind = ARG_VAR;
      arg->var  = alloc_var(&ctx->fn->vars);
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

static bool detect_arg_type(Arg *arg) {
  if (arg->type.kind != TYPE_UNKNOWN) return true;
  if (arg->kind == ARG_NONE) return true;

  TypeExpr *type = NULL;

  static_assert(__arg_kind_count == 5, "introduced more arg kinds");
  switch(arg->kind) {
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
  static_assert(__arg_kind_count == 5, "introduced more arg kinds");
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

  if (fn->is_extern) return true;

  Fn_Ctx *fn_ctx = ht_find(&ctx->known, fn);
  assert(fn_ctx != NULL);
  da_append(&ctx->prog->fn_list, fn);
  Lambda *lambda = fn_ctx->fn;

  fn->loc = lambda->loc;
  fn->type.kind = TYPE_FN;
  da_foreach(Fn_Arg, arg, &lambda->args) {
    Arg *value = scope_add(fn_ctx->sp, arg->name, arg->loc);
    if (value == NULL) return false;

    value->kind = ARG_VAR;
    value->var  = alloc_var(&ctx->fn->vars);
    value->var->type = type_clone(arg->type);

    da_append(&fn->type.fn_type.arg_types, type_clone(arg->type));
  }
  fn->type.fn_type.ret_type = arena_alloc(sizeof(TypeExpr));
  *fn->type.fn_type.ret_type = type_clone(lambda->ret_type);

  da_foreach (Stat, stat, &lambda->body) {
    if (!stat_to_ir(stat, fn_ctx->sp, ctx)) return false;
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
  if (!detect_all_unknown_type(prog)) return NULL;
  if (!check_type(prog))              return NULL;
  if (!check_fn_returned(prog))       return NULL;

  return prog;
}

