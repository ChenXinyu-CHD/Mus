#ifndef MCC_IR_H
#define MCC_IR_H

#include <stddef.h>

#include "3rd_wrapper.h"

#include "lexer.h"
#include "ast.h"
#include "type.h"

typedef enum {
  ARG_NONE = 0,
  ARG_FN,
  ARG_EXT,
  ARG_REG,
  ARG_GLOBAL_VAR,
  ARG_LIT_INT,
  ARG_LIT_STR,
  __arg_kind_count,
} ArgKind;

typedef struct Fn Fn;
typedef struct Var Var;

typedef struct {
  TypeExpr type;
  size_t id;
  bool fn_arg;
} Reg;

typedef struct {
  Reg *items;
  size_t count;
  size_t capacity;
} RegList;

typedef struct {
  Var **items;
  size_t memsize;

  size_t count;
  size_t capacity;
} VarList;

typedef struct {
  ArgKind kind;
  TypeExpr type;
  Cursor loc;
  union {
    int num_int;
    Fn *fn;
    Var *var;
    Reg reg;
    // TODO: support external variables
    String_View ext;
    size_t str_label;
  };
} Arg;

void dump_arg(String_Builder *sb, Arg *arg);

typedef struct {
  Arg *items;
  size_t count;
  size_t capacity;
} ArgList;

struct Var {
  TypeExpr type;
  String_View name;
  Arg init_value;

  ptrdiff_t offset;
};

typedef enum {
  OP_INVOKE = 0,
  OP_RETURN,
  OP_ALLOCA,
  OP_DEALLOC,
  OP_STORE,
  OP_LOAD,
  OP_SET_REG,
  OP_BINOP,
  OP_JMP,
  OP_JMP_ELSE,
  OP_LABEL,
  __op_kind_count,
} OpKind;

typedef struct {
  Arg fn;
  ArgList args;
  bool ret_ignore;
  Reg ret;
} OpInvoke;

typedef struct {
  BinopKind kind;
  Arg lhs;
  Arg rhs;
  Reg dst;
} OpBinop;

typedef struct {
  size_t label;
  Arg cond;
} OpJmp;

typedef struct {
  Reg reg;
  Arg arg;
} RegOp;

const char *binop_name(BinopKind kind);

typedef struct {
  OpKind kind;
  Cursor loc;
  union {
    OpInvoke invoke;
    Arg ret_val;
    OpBinop binop;
    OpJmp jmp;
    size_t label;
    struct {
      Reg reg;
      size_t memsize;
    } alloca;
    struct {
      Reg src;
      Reg dst;
    } load;
    RegOp store;
    RegOp set_reg;
  };
} Op;

void dump_op(String_Builder *sb, Op *op);

typedef struct {
  Op *items;
  size_t count;
  size_t capacity;
} OpList;

struct Fn {
  // functions have multiple names.
  // consider this case:
  // ```
  // let foo = fn () -> i32 {
  //    printf("foo\n");
  // }
  // let main = foo;
  // ```
  // `main` function is expected to be a alian of `foo` in assembly level.
  struct {
    String_Builder *items;
    size_t count;
    size_t capacity;
  } names;
  TypeExpr type;
  Cursor loc;

  OpList fn_body;
  RegList regs;
};

typedef struct {
  Fn **items;
  size_t count;
  size_t capacity;
} FnList;

typedef struct {
  FnList fn_list;
  VarList vars;
  struct {
    String_View *items;
    size_t count;
    size_t capacity;
  } str_lits;
} Program;

Program *compile_program(Lexer *l);

#endif // MCC_IR_H

#ifdef MCC_IR_IMPLEMENTATION

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

static bool subscope_of(Scope *sp, Scope *upper)
{
  while (sp != NULL) {
    if (sp == upper) return true;
    sp = sp->upper;
  }

  return false;
}

static Arg *scope_add(Scope *sp, String_View name, Cursor loc)
{
  Arg *value = ht_find(&sp->values, name);
  if (value != NULL) {
    pcompile_info(loc,
                  "error: symbol "SV_Fmt" redefined in this scope\n",
                  SV_Arg(name));
    pcompile_info(value->loc,
                  "info: "SV_Fmt" is first defined here.\n",
                  SV_Arg(name));
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

static Var *alloc_var(VarList *vars, String_View name, TypeExpr type)
{
  Var *var = arena_alloc(sizeof(Var));
  *var = (Var) {
    .name      = name,
    .type      = type,
  };
  da_append(vars, var);
  return var;
}

static Reg alloc_reg(RegList *regs, TypeExpr type, bool fn_arg)
{
  Reg reg = {
    .id = regs->count,
    .type = type,
    .fn_arg = fn_arg
  };
  da_append(regs, reg);
  return reg;
}

static void dump_reg(String_Builder *sb, Reg reg)
{
  sb_appendf(sb, "reg[%ld]", reg.id);
}

void dump_arg(String_Builder *sb, Arg *arg)
{
  static_assert(__arg_kind_count == 7, "introduced more arg kinds");
  switch(arg->kind) {
  case ARG_NONE:
    sb_appendf(sb, "None");
    break;
  case ARG_FN:
    sb_appendf(sb, SV_Fmt, SV_Arg(sb_to_sv(da_first(&arg->fn->names))));
    break;
  case ARG_EXT:
    sb_appendf(sb, SV_Fmt, SV_Arg(arg->ext));
    break;
  case ARG_REG:
    dump_reg(sb, arg->reg);
    break;
  case ARG_LIT_INT:
    sb_appendf(sb, "%d", arg->num_int);
    break;
  case ARG_LIT_STR:
    sb_appendf(sb, ".S_%ld", arg->str_label);
    break;
  case ARG_GLOBAL_VAR:
    sb_appendf(sb, SV_Fmt, SV_Arg(arg->var->name));
    break;
  default: UNREACHABLE("");
  }
}

void dump_op(String_Builder *sb, Op *op)
{
  static_assert(__op_kind_count == 11, "introduced more op kinds");
  switch (op->kind) {
  case OP_SET_REG:
    dump_reg(sb, op->set_reg.reg);
    sb_appendf(sb, " = ");
    dump_arg(sb, &op->set_reg.arg);
    break;
  case OP_LOAD:
    dump_reg(sb, op->load.dst);
    sb_appendf(sb, " = *");
    dump_reg(sb, op->load.src);
    break;
  case OP_STORE:
    sb_appendf(sb, "*");
    dump_reg(sb, op->store.reg);
    sb_appendf(sb, " = ");
    dump_arg(sb, &op->store.arg);
    break;
  case OP_DEALLOC:
    TODO("");
    break;
  case OP_ALLOCA:
    dump_reg(sb, op->alloca.reg);
    sb_appendf(sb, " = @alloca %ld", op->alloca.memsize);
    break;
  case OP_INVOKE:
    if (!op->invoke.ret_ignore) {
      dump_reg(sb, op->invoke.ret);
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
  case OP_BINOP:
    dump_reg(sb, op->binop.dst);
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

typedef struct {
  Lambda *fn;
  Scope *sp;
} Fn_Ctx;

typedef Ht(Fn*, Fn_Ctx) Known_Fn;

typedef struct {
  Program *prog;
  Fn *fn;
  Scope *fn_scope;

  FnList ungenerated;
  Known_Fn known;
  struct {
    size_t *items;
    size_t count;
    size_t capacity;
  } breaks;
} Gen_Context;

static Fn *push_fn(Lambda* lambda, Gen_Context *ctx, Scope *sp)
{
  Fn *fn = arena_calloc(1, sizeof(*fn));
  da_append(&ctx->ungenerated, fn);

  // default name of a function is defined here
  // and stat_to_ir may generate the other names for it
  String_Builder name = {0};
  sb_appendf(&name, ".lambda_%ld", ctx->known.count);
  da_append(&fn->names, name);
  fn->type = type_of_fn(&lambda->ret_type, &lambda->args);

  *ht_put(&ctx->known, fn) = (Fn_Ctx) {
    .fn = lambda,
    .sp = new_scope(sp),
  };
  return fn;
}

static Arg *scope_get(String_View name, Cursor loc, Scope *sp, Gen_Context *ctx)
{
  SymSearchResult r = sym_search(sp, name);
  if (r.scope == NULL) {
    pcompile_info(loc,
                  "error: cannot find `"SV_Fmt"` in this scope\n",
                  SV_Arg(name));
    return false;
  }

  if (r.value->kind == ARG_REG && !subscope_of(sp, ctx->fn_scope)) {
    pcompile_info(loc,
                  "error: `"SV_Fmt"` is not visible in this scope "
                  "because this language does not support closure.\n",
                  SV_Arg(name));
    pcompile_info(r.value->loc,
                  "info: `"SV_Fmt"` is defined in here\n",
                  SV_Arg(name));
    return false;
  }

  return r.value;
}

static bool get_id_addr(String_View name, Cursor loc, Reg *reg, Scope *sp, Gen_Context *ctx)
{
  Arg *arg = scope_get(name, loc, sp, ctx);
  if (arg == NULL) return false;

  switch (arg->kind) {
  case ARG_REG:
    if (arg->reg.fn_arg) {
      TODO("the argument of a function is not stored in memory, "
           "but logically can be referenced as a pointer. "
           "Here should introduce a local variale to replace the argument.");
      return false;
    }
    *reg = arg->reg;
    assert(arg->reg.type.kind == TYPE_PTR);
    return true;
  case ARG_GLOBAL_VAR: case ARG_FN: {
    *reg = alloc_reg(&ctx->fn->regs, type_ptr(arg->type, true), false);
    Op set_reg = {
      .kind = OP_SET_REG,
      .set_reg = {
        .reg = *reg,
        .arg = *arg,
      },
    };
    da_append(&ctx->fn->fn_body, set_reg);
    return true;
  }
  default:
    pcompile_info(loc, "error: cannot get address of `"SV_Fmt"`\n");
    return false;
  }
}

static bool get_id_value(String_View name, Cursor loc, Arg *arg, Scope *sp, Gen_Context *ctx)
{
  Arg *searched = scope_get(name, loc, sp, ctx);
  if (searched == NULL) return false;
  *arg = *searched;

  switch (arg->kind) {
  case ARG_FN:
    return true;
  case ARG_REG:
    if (arg->reg.fn_arg) return true;
    [[fallthrough]];
  case ARG_GLOBAL_VAR: {
    Reg src = {0};
    if (!get_id_addr(name, loc, &src, sp, ctx)) UNREACHABLE("it must be a bug");
    assert(src.type.kind == TYPE_PTR);
    Reg dst = alloc_reg(&ctx->fn->regs, *src.type.ptr.inner, false);
    Op load = {
      .kind = OP_LOAD,
      .load = {
        .dst = dst,
        .src = src,
      },
    };
    da_append(&ctx->fn->fn_body, load);
    arg->kind = ARG_REG;
    arg->type = dst.type;
    arg->reg  = dst;
    return true;
  }
  default:
    return true;
  }
}

static bool invoke_available(Cursor loc, AST_Invoke *invoke, Scope *sp);

static bool detect_binop_type(Expr *expr, Scope *sp, TypeExpr expected);

static bool is_type_int(TypeKind kind)
{
  return kind == TYPE_UINT || kind == TYPE_INT;
}

static void detect_intlit_type(Expr *expr, TypeExpr expected)
{
  assert(expr->kind == EXPR_INT);
  if (expr->type.kind != TYPE_UNKNOWN) return;
  if (is_type_int(expected.kind)) {
    expr->type = expected;
  } else {
    expr->type = type_int(TYPE_INT, 4);
  }
}

static bool detect_expr_type(Expr *expr, Scope *sp, TypeExpr expected)
{
  if (expr->type.kind == TYPE_UNKNOWN) {
    static_assert(__expr_kind_count == 9, "introduced more expr kinds");
    switch (expr->kind) {
    case EXPR_REF:
      if (!detect_expr_type(expr->ref.inner, sp, type_unknown())) return false;
      expr->type = type_ptr(expr->ref.inner->type, expr->ref.mutable);
      break;
    case EXPR_DREF:
      if (!detect_expr_type(expr->deref, sp, type_unknown())) return false;
      if (expr->deref->type.kind != TYPE_PTR) {
        pcompile_info(expr->loc,
                      "error: this expression is not a pointer, "
                      "and cannot be dereferenced.\n");
        return false;
      }
      expr->type = *expr->deref->type.ptr.inner;
      break;
    case EXPR_BINOP:
      if (!detect_binop_type(expr, sp, expected)) return false;
      break;
    case EXPR_NAME: {
      SymSearchResult r = sym_search(sp, expr->name);
      if (r.scope == NULL) {
        pcompile_info(expr->loc,
                      "error: cannot find `"SV_Fmt"` in this scope\n",
                      SV_Arg(expr->name));
        return NULL;
      }
      if (r.value->kind == ARG_REG && !r.value->reg.fn_arg) {
        assert(r.value->type.kind == TYPE_PTR);
        expr->type = *r.value->type.ptr.inner;
      } else {
        expr->type = r.value->type;
      }
    } break;
    case EXPR_INVOKE:
      if (!invoke_available(expr->loc, &expr->invoke, sp)) return false;
      expr->type = *expr->invoke.fn->type.fn.ret;
      break;
    case EXPR_INT:
      detect_intlit_type(expr, expected);
      break;
    case EXPR_ARR_INIT: {
      TypeExpr item_expected = expected.kind == TYPE_ARRAY?
        *expected.arr.inner : type_unknown();
      assert(expr->arr_init.count != 0);
      if (!detect_expr_type(&da_first(&expr->arr_init), sp, item_expected))
        return false;
      // this is useful if item_expected.kind == TYPE_UNKNOWN.
      // Otherwise, it is useless because
      // da_first(&expr->arr_init).type == item_expected.
      item_expected = da_first(&expr->arr_init).type;
      if (!expr->arr_init.repeat) {
        for (size_t i = 1; i < expr->arr_init.count; ++i) {
          if (!detect_expr_type(&expr->arr_init.items[i], sp, item_expected))
            return false;
        }
      }
      expr->type.kind = TYPE_ARRAY;
      expr->type.arr.inner = &da_first(&expr->arr_init).type;
      expr->type.arr.count = expr->arr_init.count;
    } break;
    case EXPR_STR:
    case EXPR_LAMBDA:
      assert(false && "these type must be known, it may be a bug in ast.h");
      break;
    default: UNREACHABLE("");
    }
  }

  if (expr->type.kind == TYPE_UNKNOWN) {
    pcompile_info(expr->loc, "error: cannot detect the type of this expression\n");
    return false;
  }

  if (expected.kind != TYPE_UNKNOWN && !type_eq(&expected, &expr->type)) {
    pcompile_info(expr->loc, "error: here expected a ");
    dump_type_expr(&expected, stderr);
    fprintf(stderr, ", but got ");
    dump_type_expr(&expr->type, stderr);
    fputc('\n', stderr);
    return false;
  }
  return true;
}

static bool is_compiletime_binop(Expr *expr) {
  if (expr->kind != EXPR_BINOP) return false;
  Expr *lhs = expr->binop.lhs;
  Expr *rhs = expr->binop.rhs;
  return
    (lhs->kind == EXPR_INT || is_compiletime_binop(lhs)) &&
    (rhs->kind == EXPR_INT || is_compiletime_binop(rhs));
}

static bool detect_compiletime_binop_type(Expr *expr, TypeExpr expected)
{
  if (expr->kind == EXPR_INT) {
    detect_intlit_type(expr, expr->type);
    return true;
  }

  assert(is_compiletime_binop(expr));
  Expr *lhs = expr->binop.lhs;
  Expr *rhs = expr->binop.rhs;
  bool ok = true;
  switch(expr->binop.kind) {
  case BINOP_MUL:
  case BINOP_DIV:
  case BINOP_MOD:
  case BINOP_ADD:
  case BINOP_SUB:
    expr->type = is_type_int(expected.kind) ? expected : type_int(TYPE_INT, 4);
    if (!detect_compiletime_binop_type(lhs, expr->type)) ok = false;
    if (!detect_compiletime_binop_type(rhs, expr->type)) ok = false;
    break;
  case BINOP_EQ:
  case BINOP_NEQ:
  case BINOP_LS:
  case BINOP_GT:
  case BINOP_LE:
  case BINOP_GE:
    expr->type = type_bool();
    if (!detect_compiletime_binop_type(lhs, type_unknown())) ok = false;
    if (!detect_compiletime_binop_type(rhs, type_unknown())) ok = false;
    break;
  default: UNREACHABLE("");
  }
  if (!type_eq(&lhs->type, &rhs->type)) {
    pcompile_info(expr->loc,
                  "error: operator %s between type ",
                  binop_name(expr->binop.kind));
    dump_type_expr(&lhs->type, stderr);
    fprintf(stderr, " and ");
    dump_type_expr(&rhs->type, stderr);
    fprintf(stderr, " are not supported.\n");
    return false;
  }
  return ok;
}

static bool detect_binop_type(Expr *expr, Scope *sp, TypeExpr expected)
{
  assert(expr->kind == EXPR_BINOP && expr->type.kind == TYPE_UNKNOWN);
  bool ok = true;

  if (is_compiletime_binop(expr)) {
    return detect_compiletime_binop_type(expr, expected);
  }

  Expr *lhs = expr->binop.lhs;
  Expr *rhs = expr->binop.rhs;

  bool supported = false;
  static_assert(__binop_kind_count == 11, "introduced more binop kinds");
  switch(expr->binop.kind) {
  case BINOP_MUL:
  case BINOP_DIV:
  case BINOP_MOD: {
    if (lhs->kind != EXPR_INT) {
      if (!detect_expr_type(lhs, sp, type_unknown())) ok = false;
      if (!detect_expr_type(rhs, sp, lhs->type)) ok = false;
    } else {
      if (!detect_expr_type(rhs, sp, type_unknown())) ok = false;
      if (!detect_expr_type(lhs, sp, rhs->type)) ok = false;
    }
    supported = is_type_int(lhs->type.kind) && type_eq(&lhs->type, &rhs->type);
    expr->type = lhs->type;
  } break;
  case BINOP_ADD:
  case BINOP_SUB: {
    if (lhs->kind != EXPR_INT) {
      if (!detect_expr_type(lhs, sp, type_unknown())) ok = false;
      if (lhs->type.kind == TYPE_PTR) {
        if (!detect_expr_type(rhs, sp, type_unknown())) ok = false;
      } else {
        if (!detect_expr_type(rhs, sp, lhs->type)) ok = false;
      }
    } else {
      if (!detect_expr_type(rhs, sp, type_unknown())) ok = false;
      if (rhs->type.kind == TYPE_PTR) {
        if (!detect_expr_type(lhs, sp, type_unknown())) ok = false;
      } else {
        if (!detect_expr_type(lhs, sp, rhs->type)) ok = false;
      }
    }
    supported =
      (is_type_int(lhs->type.kind) && type_eq(&lhs->type, &rhs->type)) ||
      (is_type_int(lhs->type.kind) && rhs->type.kind == TYPE_PTR) ||
      (lhs->type.kind == TYPE_PTR && is_type_int(rhs->type.kind));
    expr->type = lhs->type.kind == TYPE_PTR? lhs->type : rhs->type;
  } break;
  case BINOP_EQ:
  case BINOP_NEQ:
  case BINOP_LS:
  case BINOP_GT:
  case BINOP_LE:
  case BINOP_GE: {
    if (lhs->kind != EXPR_INT) {
      if (!detect_expr_type(lhs, sp, type_unknown())) ok = false;
      if (!detect_expr_type(rhs, sp, lhs->type)) ok = false;
    } else {
      if (!detect_expr_type(rhs, sp, type_unknown())) ok = false;
      if (!detect_expr_type(lhs, sp, rhs->type)) ok = false;
    }
    supported = type_eq(&lhs->type, &rhs->type);
    expr->type = type_bool();
  } break;
  default: UNREACHABLE("");
  }
  if (!supported) {
    pcompile_info(expr->loc,
                  "error: operator %s between type ",
                  binop_name(expr->binop.kind));
    dump_type_expr(&lhs->type, stderr);
    fprintf(stderr, " and ");
    dump_type_expr(&rhs->type, stderr);
    fprintf(stderr, " are not supported.\n");
    return false;
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

  FnType fn_type = invoke->fn->type.fn;
  bool arg_matched = true;
  if (fn_type.args.count > invoke->args.count)
    arg_matched = false;
  if (!fn_type.va_args && fn_type.args.count < invoke->args.count)
    arg_matched = false;
  if (!arg_matched) {
    pcompile_info(loc,
                  "error: this function expects %ld arguments, but provided %ld.\n",
                  fn_type.args.count,
                  invoke->args.count);
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < invoke->args.count; ++ i) {
    TypeExpr expect = type_unknown();
    if (i < fn_type.args.count)
      expect = fn_type.args.items[i];

    if (!detect_expr_type(&invoke->args.items[i], sp, expect)) ok = false;
  }
  return ok;
}

static bool expr_to_ir(Expr *expr, Scope *sp, Gen_Context *ctx);

static bool expr_to_arg(Expr *expr, Scope *sp, Gen_Context *ctx, Arg *result)
{
  result->loc  = expr->loc;
  result->type = expr->type;

  static_assert(__expr_kind_count == 9, "introduced more expr kinds");
  switch (expr->kind) {
  case EXPR_DREF: {
    if (!expr_to_ir(expr, sp, ctx)) return false;
    Op *load = &da_last(&ctx->fn->fn_body);
    assert(load->kind == OP_LOAD);
    result->kind = ARG_REG;
    result->reg = load->load.dst;
    return true;
  }
  case EXPR_REF: {
    Expr *refered = expr->ref.inner;
    if (refered->kind == EXPR_NAME) {
      Reg addr = {0};
      if (!get_id_addr(refered->name, refered->loc, &addr, sp, ctx))
        return false;
      result->kind = ARG_REG;
      result->reg = addr;
    } else if (refered->kind == EXPR_DREF) {
      if (!expr_to_arg(refered->deref, sp, ctx, result)) return false;
    } else {
      pcompile_info(expr->loc, "error: this cannot be referenced.\n");
    }
    return true;
  }
  case EXPR_NAME: {
    if (!get_id_value(expr->name, expr->loc, result, sp, ctx)) return false;
    return true;
  }
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
    op->invoke.ret = alloc_reg(&ctx->fn->regs, expr->type, false);
    *result = (Arg) {
      .kind = ARG_REG,
      .reg  = op->invoke.ret,
      .type = op->invoke.ret.type,
    };
    return true;
  }
  case EXPR_BINOP: {
    if (!expr_to_ir(expr, sp, ctx)) return false;

    Op *op = &da_last(&ctx->fn->fn_body);
    assert(op->kind == OP_BINOP);

    *result = (Arg) {
      .kind = ARG_REG,
      .type = op->binop.dst.type,
      .reg  = op->binop.dst,
    };
    return true;
  }
  case EXPR_LAMBDA: {
    result->kind = ARG_FN;
    result->fn   = push_fn(&expr->lambda, ctx, sp);
    return true;
  }
  case EXPR_ARR_INIT:
    TODO("");
    break;
  default: UNREACHABLE("");
  }
  return true;
}

static bool expr_to_ir(Expr *expr, Scope *sp, Gen_Context *ctx)
{
  Op op = { .loc = expr->loc };
  static_assert(__expr_kind_count == 9, "introduced more expr kinds");
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

    op.binop.dst = alloc_reg(&ctx->fn->regs, expr->type, false);
    da_append(&ctx->fn->fn_body, op);
  } break;
  case EXPR_DREF: {
    Arg src = {0};
    if (!expr_to_arg(expr->deref, sp, ctx, &src))
      return false;
    assert(src.type.kind == TYPE_PTR);
    assert(src.kind == ARG_REG);
    Reg dst = alloc_reg(&ctx->fn->regs, *src.type.ptr.inner, false);
    op.kind = OP_LOAD;
    op.load.dst = dst;
    op.load.src = src.reg;
    da_append(&ctx->fn->fn_body, op);
  } break;
  case EXPR_ARR_INIT:
    TODO("");
    break;
  case EXPR_REF:
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
  val->type = expr->type;
  static_assert(__expr_kind_count == 9, "introduced more expr kinds");
  switch (expr->kind) {
  case EXPR_DREF:
  case EXPR_REF:
    pcompile_info(expr->loc,
                  "error: pointer is not allowed in compile time\n");
    return false;
  case EXPR_INVOKE:
    pcompile_info(expr->loc,
                  "error: invoking a function is not allowed in compile time\n");
    return false;
  case EXPR_INT:
  case EXPR_STR:
  case EXPR_LAMBDA:
    return expr_to_arg(expr, sp, ctx, val);
  case EXPR_ARR_INIT:
    TODO("It is ok logically, but currently I can't implement it. "
         "Maybe I need to ");
    return false;
  case EXPR_NAME:
    if (!expr_to_arg(expr, sp, ctx, val)) return false;
    if (val->kind == ARG_GLOBAL_VAR || val->kind == ARG_REG) {
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
  static_assert(__stat_kind_count == 9, "introduced more stat kinds");
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
    if (stat->ret_val != NULL) {
      if (!detect_expr_type(stat->ret_val, sp, *ctx->fn->type.fn.ret))
        return false;
      if (!expr_to_arg(stat->ret_val, sp, ctx, &op.ret_val))
        return false;
    } else {
      op.ret_val = (Arg) {.kind = ARG_NONE};
    }
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
  case STAT_FOR: {
    Scope *new_sp = new_scope(sp);
    if (stat->for_loop.init != NULL) {
      if (!stat_to_ir(stat->for_loop.init, new_sp, ctx)) return false;
    }

    size_t loop_label = append_op_label(&ctx->fn->fn_body);

    size_t break_begin = ctx->breaks.count;
    if (stat->for_loop.cond != NULL) {
      if (!detect_expr_type(stat->for_loop.cond, new_sp, type_bool()))
        return false;
      Arg cond = {0};
      if (!expr_to_arg(stat->for_loop.cond, new_sp, ctx, &cond)) return false;

      size_t jmp_break = append_op(&ctx->fn->fn_body, (Op) {
          .kind = OP_JMP_ELSE,
          .jmp = {.cond = cond},
        });
      da_append(&ctx->breaks, jmp_break);
    }

    assert(stat->for_loop.body != NULL);
    if (!stat_to_ir(stat->for_loop.body, new_sp, ctx)) return false;

    if (stat->for_loop.update != NULL) {
      if (!stat_to_ir(stat->for_loop.update, new_sp, ctx)) return false;
    }

    append_op(&ctx->fn->fn_body, (Op) {
        .kind = OP_JMP,
        .jmp = {.label = loop_label},
      });
    size_t break_label = append_op_label(&ctx->fn->fn_body);
    for (size_t i = break_begin; i < ctx->breaks.count; ++i) {
      size_t break_jmp = ctx->breaks.items[i];
      Op *op = &ctx->fn->fn_body.items[break_jmp];
      assert(op->kind == OP_JMP || op->kind == OP_JMP_ELSE);
      op->jmp.label = break_label;
    }
    ctx->breaks.count = break_begin;
  } break;
  case STAT_BREAK: {
    size_t break_jmp = append_op(&ctx->fn->fn_body, (Op) {
        .kind = OP_JMP,
      });
    da_append(&ctx->breaks, break_jmp);
  } break;
  case STAT_ASSIGN: {
    if (!detect_expr_type(stat->assign.dst, sp, type_unknown())) return false;

    TypeExpr expected = stat->assign.dst->type;
    assert(expected.kind != TYPE_UNKNOWN &&
           "the type of the destination in assignment must be known, "
           "this may be a bug in stat_to_ir ot detect_expr_type.");
    if (!detect_expr_type(stat->assign.val, sp, expected)) return false;
    Arg src = {0};
    if (!expr_to_arg(stat->assign.val, sp, ctx, &src))
      return false;

    Reg dst = {0};
    if (stat->assign.dst->kind == EXPR_NAME) {
      if (!get_id_addr(stat->assign.dst->name, stat->loc, &dst, sp, ctx))
        return false;
    } else if (stat->assign.dst->kind == EXPR_DREF) {
      Arg arg = {0};
      if (!expr_to_arg(stat->assign.dst->deref, sp, ctx, &arg))
        return false;
      assert(arg.kind == ARG_REG);
      dst = arg.reg;
    } else {
      pcompile_info(stat->assign.dst->loc, "error: this is not assignable.\n");
      return false;
    }
    assert(dst.type.kind == TYPE_PTR);
    op.kind = OP_STORE;
    op.store.arg = src;
    op.store.reg = dst;
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
      stat->def.type = stat->def.val->type;
    } else if (stat->def.val != NULL) {
      if (!detect_expr_type(stat->def.val, sp, stat->def.type)) return false;
    }
    static_assert(__def_kind_count == 2,
                  "introduced more def kinds");
    switch (stat->def.kind) {
    case DEF_LET: {
      Arg val = {0};
      if (stat->def.is_extern) {
        assert(stat->def.val == NULL);
        val = (Arg) {
          .kind = ARG_EXT,
          .type = stat->def.type,
          .ext  = stat->def.name,
        };
      } else {
        assert(stat->def.val != NULL);
        if (!expr_eval(stat->def.val, sp, ctx, &val)) return false;
      }

      Arg *arg = scope_add(sp, stat->def.name, stat->loc);
      if (arg == NULL) return false;
      *arg = val;

      // only global `let` bindings and need a meaningful name.
      if (arg->kind == ARG_FN && ctx->fn == NULL) {
        String_Builder name = {0};
        sb_append_sv(&name, stat->def.name);
        da_append(&arg->fn->names, name);
      }
    } break;
    case DEF_VAR: {
      if (ctx->fn == NULL) { // global variable
        Var *var = alloc_var(&ctx->prog->vars, stat->def.name, stat->def.type);
        if (stat->def.val != NULL) {
          if (!expr_eval(stat->def.val, sp, ctx, &var->init_value))
            return false;
        }
        Arg *arg = scope_add(sp, stat->def.name, stat->loc);
        if (arg == NULL) return false;
        arg->kind = ARG_GLOBAL_VAR;
        arg->var  = var;
        arg->type = stat->def.type;
      } else { // local variable
        TypeExpr type = type_ptr(stat->def.type, true);
        Reg reg = alloc_reg(&ctx->fn->regs, type, false);
        Op alloca = {
          .kind = OP_ALLOCA,
          .loc = stat->loc,
          .alloca = {
            .reg = reg,
            .memsize = stat->def.type.size,
          }
        };
        da_append(&ctx->fn->fn_body, alloca);
        if (stat->def.val != NULL) {
          op.kind = OP_STORE;
          op.store.reg = reg;
          if (!expr_to_arg(stat->def.val, sp, ctx, &op.store.arg)) return false;
          da_append(&ctx->fn->fn_body, op);
        }
        Arg *arg = scope_add(sp, stat->def.name, stat->loc);
        if (arg == NULL) return false;
        arg->kind = ARG_REG;
        arg->reg  = reg;
        arg->type = reg.type;
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

  Fn_Ctx *fn_ctx = ht_find(&ctx->known, fn);
  assert(fn_ctx != NULL);
  ctx->fn_scope = fn_ctx->sp;
  da_append(&ctx->prog->fn_list, fn);
  Lambda *lambda = fn_ctx->fn;

  fn->loc = lambda->loc;
  da_foreach(Fn_Arg, arg, &lambda->args) {
    Arg *value = scope_add(fn_ctx->sp, arg->name, arg->loc);
    if (value == NULL) return false;

    value->kind = ARG_REG;
    value->reg  = alloc_reg(&ctx->fn->regs, arg->type, true);
    value->type = arg->type;
  }

  da_foreach (Stat, stat, &lambda->body) {
    if (!stat_to_ir(stat, fn_ctx->sp, ctx)) return false;
  }
  assert(ctx->breaks.count == 0);

  if (fn->fn_body.count == 0 || da_last(&fn->fn_body).kind != OP_RETURN) {
    da_append(&fn->fn_body, ((Op) {
        .kind = OP_RETURN,
        .ret_val = {.kind = ARG_NONE},
        }));
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

#endif // MCC_IR_IMPLEMENTATION
