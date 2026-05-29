#include "parser.h"

#include "3rd/nob.h"
#include "3rd/ht.h"

#include "lexer.h"
#include "utils.h"
#include "type.h"
#include "ast.h"
#include "SymbolTable.h"

Var *alloc_var(VarList *vars)
{
  Var *var = malloc(sizeof(Var));
  *var = (Var) {
    .type = {.kind = TYPE_UNKNOWN}
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

  if (fn->args.count != 0) {
    for (size_t i = 0; i < fn->args.count; ++i) {
      Var *arg = fn->args.items[i];
      destroy_type_expr(&arg->type);
      free(arg);
    }
    da_free(fn->args);
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

typedef Ht(AST_Fn*, Fn*) Known_Fn;
typedef struct {
  AST_Fn **items;
  size_t capacity;
  size_t count;
} AST_Fn_List;

typedef struct {
  Program *prog;
  Fn *fn;

  AST_Fn_List ungenerated;
  Known_Fn known;
} Gen_Context;

void push_fn_ast(Gen_Context *ctx, String_Builder name, AST_Fn* ast)
{
  assert(ht_find(&ctx->known, ast) == NULL);
  Fn *fn = calloc(1, sizeof(*fn));
  fn->name = name;
  da_append(&ctx->ungenerated, ast);
  *ht_put(&ctx->known, ast) = fn;
}

static bool id_to_arg(Token id, Arg *arg, Scoop *sp, Gen_Context *ctx)
{
  assert(id.kind == TOKEN_ID);

  arg->loc = id.start;
  SymSearchResult r = sym_search(sp, id.str);
  if (r.scoop == NULL) {
    pcompile_info(id.start,
                  "error: cannot find `"SV_Fmt"` in this scoop\n",
                  SV_Arg(id.str));
    return false;
  }

  static_assert(__symbol_kind_count == 3, "introduced more symbol kinds");
  switch (r.sym->kind) {
  case SYMBOL_VAR: {
    bool available =
      // after the variable is intialized
      cs_pos(id.start) > cs_pos(r.sym->var->init_end) ||
      // or if it is the dst of an assignment when the variable is defined
      cs_pos(id.start) == cs_pos(r.sym->var->loc);

    if (available) {
      Var* var = r.sym->var;

      if (contains(ctx->fn->vars.items, ctx->fn->vars.count, var)) {
        arg->kind = ARG_VAR;
        arg->var = var;
        arg->type = type_clone(var->type);
        return true;
      } else if (contains(ctx->fn->args.items, ctx->fn->args.count, var)) {
        arg->kind = ARG_VAR;
        arg->var = var;
        arg->type = type_clone(var->type);
        return true;
      } else {
        pcompile_info(arg->loc,
                      "error: `"SV_Fmt"` cannot be refered in this scoop\n",
                      SV_Arg(id.str));
        pcompile_info(var->loc,
                      "info: `"SV_Fmt"` is defined in here\n",
                      SV_Arg(id.str));
        return false;
      }
    } else {
      return id_to_arg(id, arg, sp->upper, ctx);
    }
  }
  case SYMBOL_EXTERN: {
    Extern *ext = r.sym->ext;
    arg->kind = ARG_EXTERN;
    arg->ext = ext;
    arg->type = type_clone(ext->type);
    return true;
  }
  case SYMBOL_FN: {
    Fn **arg_fn = ht_find(&ctx->known, r.sym->ast_fn);
    assert(arg_fn &&
           "all fn in this scoop should be inserted in fns at first");
    arg->kind = ARG_FN;
    arg->fn = *arg_fn;
    // this must be unknown, because the refered function may not be generated
    arg->type.kind = TYPE_UNKNOWN;;
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
      return id_to_arg(token, result, sp, ctx);
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

    op->invoke.result = alloc_var(&ctx->fn->vars);
    op->invoke.ret_ignore = false;
    *result = (Arg) {
      .kind = ARG_VAR,
      .type = {.kind = TYPE_UNKNOWN},
      .var = op->invoke.result,
    };
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

static bool stat_to_ir(Stat *stat, Scoop *sp, Gen_Context *ctx)
{
  Op op = { .loc = stat->loc };
  static_assert(__stat_kind_count == 6, "introduced more stat kinds");
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
  case STAT_BLOCK:
    //    da_append(&prog->symbols, stat->block.local);
    ht_foreach(sym, &stat->block.local->symbols) {
      if (sym->kind == SYMBOL_VAR) {
        da_append(&ctx->fn->vars, sym->var);
      } else if (sym->kind == SYMBOL_FN) {
        String_Builder name = {0};
        sb_appendf(&name, ".fn_%ld", ctx->known.count);
        push_fn_ast(ctx, name, sym->ast_fn);
      }
    }
    da_foreach(Stat, s, &stat->block.stats) {
      if (!stat_to_ir(s, stat->block.local, ctx)) return false;
    }
    break;
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
      size_t end_label = append_op(&ctx->fn->fn_body, (Op){
          .kind = OP_LABEL,
        });
      ctx->fn->fn_body.items[jmp_else].jmp.label = end_label;
    } else {
      size_t jmp_end = append_op(&ctx->fn->fn_body, (Op){
          .kind = OP_JMP,
        });

      size_t else_label = append_op(&ctx->fn->fn_body, (Op){
          .kind = OP_LABEL,
        });
      ctx->fn->fn_body.items[jmp_else].jmp.label = else_label;

      if (!stat_to_ir(stat->if_else.on_false, sp, ctx)) return false;

      size_t end_label = append_op(&ctx->fn->fn_body, (Op){
          .kind = OP_LABEL,
        });
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
          Var *ret = op->invoke.result;
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

typedef struct {
  String_View name;
  Cursor loc;
} Def;

typedef struct {
  Def *items;
  size_t count;
  size_t capacity;
} DefList;

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

static bool gen_ir_fn(AST_Fn *ast, Gen_Context *ctx)
{
  Fn **fn = ht_find(&ctx->known, ast);
  assert(fn != NULL);
  da_append(&ctx->prog->fn_list, *fn);
  ctx->fn = *fn;

  (*fn)->loc = ast->loc;
  (*fn)->type.kind = TYPE_FN;
  da_foreach(Var*, arg, &ast->args) {
    da_append(&ctx->fn->args, *arg);
    da_append(&(*fn)->type.fn_type.arg_types, type_clone((*arg)->type));
  }
  (*fn)->type.fn_type.ret_type = malloc(sizeof(TypeExpr));
  *(*fn)->type.fn_type.ret_type = type_clone(ast->ret_type);

  if (!stat_to_ir(ast->body, ast->local, ctx)) return false;
  return true;
}

static bool gen_ir(Program *prog, Scoop *global)
{
  // To make the code generation able to find all of the symbols
  // the first pass would collect all global symbols
  // and the second pass generate the actual code
  Gen_Context ctx = {.prog = prog};
  ht_foreach(sym, &global->symbols) {
    static_assert(__symbol_kind_count == 3,
                  "introduced more symbol kinds");
    switch(sym->kind) {
    case SYMBOL_FN: {
      String_Builder name = {0};
      sb_appendf(&name, SV_Fmt, SV_Arg(ht_key(&global->symbols, sym)));
      push_fn_ast(&ctx, name, sym->ast_fn);
    } break;
    case SYMBOL_VAR:
      TODO("support global variables.");
      break;
    case SYMBOL_EXTERN:
      da_append(&prog->externs, sym->ptr);
      break;
    default: UNREACHABLE("");
    }
  }

  AST_Fn_List *ungenerated = &ctx.ungenerated;
  while (ungenerated->count != 0) {
    AST_Fn *fn = da_pop(ungenerated);
    gen_ir_fn(fn, &ctx);
  }

  return true;
}

bool compile_program(Lexer *l, Program *prog)
{
  Scoop *global = new_scoop(NULL);
  if (!compile_prog_ast(l, global)) return false;

  if (!gen_ir(prog, global)) return false;

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

  //  free_all_symbol(&prog->symbols);
}


