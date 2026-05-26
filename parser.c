#include "parser.h"
#include "lexer.h"

#include "3rd/nob.h"
#include "utils.h"

#define MCC_TYPE_IMPLEMENTATION
#include "type.h"

#define MCC_AST_IMPLEMENTATION
#include "ast.h"

#define HT_IMPLEMENTATION
#include "3rd/ht.h"

bool insert_sym(Scoop *sp, Token name, SymbolKind kind, void *ptr)
{
  Symbol *sym = ht_find(&sp->symbols, name.str);
  if (sym != NULL) {
    pcompile_info(name.start,
                  "error: symbol "SV_Fmt" redefined in this scoop\n",
                  SV_Arg(name.str));
    // TODO: report where the symbol is first defined;
    return false;
  } else {
    *ht_put(&sp->symbols, name.str) = (Symbol) {
      .kind = kind,
      .ptr = ptr,
    };
    return true;
  }
}

String_View sym_name(Scoop* sp, void *sym)
{
  ht_foreach(value, &sp->symbols) {
    if (value->ptr == sym) {
      return ht_key(&sp->symbols, value);
    }
  }
  return sv_from_cstr("");
}

SymSearchResult sym_search(Scoop *sp, String_View name)
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

Scoop *alloc_scoop(SymbolTable *st, Scoop *upper)
{
  Scoop *result = malloc(sizeof(Scoop));
  assert(result && "buy more memory");

  *result = (Scoop) {
    .upper = upper,
    .symbols = {
      .hasheq = ht_sv_hasheq,
    },
  };
  da_append(st, result);
  return result;
}

void free_all_symbol(SymbolTable *st)
{
  da_foreach (Scoop*, sp, st) {
    ht_free(sp);
    free(*sp);
  }
  da_free(*st);
}

size_t alloc_var(VarList *vars)
{
  Var *var = malloc(sizeof(Var));
  *var = (Var) {
    .type = {.kind = TYPE_UNKNOWN}
  };
  da_append(vars, var);
  return vars->count - 1;
}
static void destroy_op(Op *op)
{
  static_assert(__op_kind_count == 7);
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

static Arg arg_local_var(Fn *fn, size_t i)
{
  assert(i < fn->vars.count);
  return (Arg) {
    .kind = ARG_VAR_LOC,
    .type = type_clone(fn->vars.items[i]->type),
    .label = i,
  };
}

static void ast_to_ir(AST *ast, Program *prog, Fn *fn, Scoop *sp);

static void ast_to_arg(AST *ast, Program *prog, Fn *fn, Scoop *sp, Arg *exp_result)
{

  static_assert(__ast_kind_count == 3);
  switch (ast->kind) {
  case AST_ATOM: {
    Token token = ast->atom;
    switch (token.kind) {
    case TOKEN_ID:
      exp_result->kind = ARG_NAME;
      exp_result->name = token.str;
      exp_result->scoop = sp;
      exp_result->loc = token.start;
      exp_result->type = (TypeExpr) {.kind = TYPE_UNKNOWN};

      break;
    case TOKEN_STR:
      exp_result->kind = ARG_LIT_STR;
      exp_result->label = compile_strlit(prog, token.str);
      exp_result->type = type_ptr((TypeExpr) {
          .kind = TYPE_INT,
          .size = 1,
        });
      break;
    case TOKEN_INT:
      exp_result->kind = ARG_LIT_INT;
      exp_result->num_int = sv_to_int(token.str);
      exp_result->type = (TypeExpr) {
        .kind = TYPE_INT,
        .size = 4,
      };
      break;
    case TOKEN_TRUE:
      exp_result->kind = ARG_LIT_INT;
      exp_result->num_int = 1;
      exp_result->type = type_bool();
      break;
    case TOKEN_FALSE:
      exp_result->kind = ARG_LIT_INT;
      exp_result->num_int = 0;
      exp_result->type = type_bool();
      break;
    default: UNREACHABLE("");
    }
  } break;
  case AST_INVOKE: {
    ast_to_ir(ast, prog, fn, sp);

    Op *op = &fn->fn_body.items[fn->fn_body.count-1];
    assert(op->kind == OP_INVOKE);

    op->invoke.result_label = alloc_var(&fn->vars);
    op->invoke.ret_ignore = false;
    *exp_result = (Arg) {
      .kind = ARG_VAR_LOC,
      .type = {.kind = TYPE_UNKNOWN},
      .label = op->invoke.result_label,
    };
  } break;
  case AST_BINOP: {
    ast_to_ir(ast, prog, fn, sp);

    Op *op = &fn->fn_body.items[fn->fn_body.count-1];
    assert(op->kind == OP_BINOP);

    *exp_result = op->binop.dst;
  } break;
  default: UNREACHABLE("");
  }
}

static void ast_to_ir(AST *ast, Program *prog, Fn *fn, Scoop *sp)
{
  static_assert(__ast_kind_count == 3);
  switch (ast->kind) {
  case AST_INVOKE: {
    Op op = { .kind = OP_INVOKE };
    ast_to_arg(ast->invoke.fn, prog, fn, sp, &op.invoke.fn);

    da_foreach(AST, ast_arg, &ast->invoke.args) {
      Arg arg = {0};
      ast_to_arg(ast_arg, prog, fn, sp, &arg);
      da_append(&op.invoke.args, arg);
    }

    op.invoke.ret_ignore = true;
    da_append(&fn->fn_body, op);
  } break;
  case AST_BINOP: {
    Op op = {
      .kind = OP_BINOP,
      .binop = {
        .kind = ast->binop.kind,
      },
    };

    ast_to_arg(ast->binop.lhs, prog, fn, sp, &op.binop.lhs);
    ast_to_arg(ast->binop.rhs, prog, fn, sp, &op.binop.rhs);

    op.binop.dst = (Arg) {
      .kind = ARG_VAR_LOC,
      .type = {.kind = TYPE_UNKNOWN},
      .label = alloc_var(&fn->vars),
    };

    da_append(&fn->fn_body, op);
  } break;
  case AST_ATOM: break; // this doesn't need to generate an ir op currently.
  default: UNREACHABLE("");
  }
}

static bool compile_stat_simple(Lexer *l, Program *prog, Fn *fn, Scoop *sp)
{
  Cursor loc = l->cursor;
  AST expr = {0};
  if (!compile_expr(l, &expr)) return false;

  bool result;
  if (l->current.kind == '=') {
    AST val_ast = {0};
    if (!prefetch_not_none(l)) return_defer(false);
    if (!compile_expr(l, &val_ast)) return_defer(false);

    Arg var, val;
    ast_to_arg(&expr, prog, fn, sp, &var);
    ast_to_arg(&val_ast, prog, fn, sp, &val);

    Op set_var = {
      .kind = OP_SET_VAR,
      .loc = loc,
      .set_var = {
        .var = var,
        .val = val,
      },
    };
    da_append(&fn->fn_body, set_var);

    ast_del(&val_ast);
  } else {
    ast_to_ir(&expr, prog, fn, sp);
  }

  return_defer(true);
 defer:
  ast_del(&expr);
  return result;
}

static_assert(__type_kind_count == 7);
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

static bool compile_local_var_singn(Lexer *l, Fn *fn, Scoop *sp)
{
  if (!prefetch_expect_token(l, TOKEN_ID)) return false;
  
  Var *var = calloc(1, sizeof(Var));
  assert(var);
  var->loc = l->current.start;
  da_append(&fn->vars, var);
  if (!insert_sym(sp, l->current, SYMBOL_VAR, var)) return false;

  if (!prefetch_not_none(l)) return false;

  if (l->current.kind == ':') {
    if (!prefetch_not_none(l)) return false;
    if (!compile_type_expr(l, &var->type)) return false;
    if (var->type.kind == TYPE_VOID) {
      pcompile_info(l->current.start, "error: the type of a local variable cannot be \"void\"");
      return false;
    }
    if (!prefetch_not_none(l)) return false;
  }
  
  return true;
}

static bool compile_local_var(Lexer *l, Program *prog, Fn *fn, Scoop *sp)
{
  if (!compile_local_var_singn(l, fn, sp)) return false;
  size_t pos = fn->vars.count - 1;

  if (l->current.kind != '=') return true;

  Cursor loc = l->current.start;
  if (!prefetch_not_none(l)) return false;
  AST expr = {0};
  if (!compile_expr(l, &expr)) return false;
  Arg val = {0};
  ast_to_arg(&expr, prog, fn, sp, &val);
  ast_del(&expr);

  Op set_var = {
    .kind = OP_SET_VAR,
    .loc = loc,
    .set_var = {
      .var = arg_local_var(fn, pos),
      .val = val,
    },
  };
  da_append(&fn->fn_body, set_var);

  if (l->current.kind == ';') {
    if (!prefetch_not_none(l)) return false;
  }

  return true;
}

static bool compile_block(Lexer *l, Program* prog, Fn *fn, Scoop *sp);

static bool compile_stat(Lexer *l, Program *prog, Fn *fn, Scoop *sp)
{
  // block
  if (l->current.kind == '{') {
    return compile_block(l, prog, fn, sp);
  }

  // empty statement;
  if (l->current.kind == ';') {
    if (!prefetch_not_none(l)) return false;
    return true;
  }

  // simple statement
  if (l->current.kind == TOKEN_RET) {
    Op ret = {
      .kind = OP_RETURN,
      .loc = l->current.start,
    };
    if (!prefetch_not_none(l)) return false;

    AST expr = {0};
    if (!compile_expr(l, &expr)) return false;
    ast_to_arg(&expr, prog, fn, sp, &ret.ret_val);
    ast_del(&expr);

    da_append(&fn->fn_body, ret);
  } else if (l->current.kind == TOKEN_IF) {
    if (!prefetch_not_none(l)) return false;

    { // parse cond, and jump to else branch
      Op op = {
        .kind = OP_JMP_ELSE,
      };
      AST cond = {0};
      if (!compile_expr(l, &cond)) return false;
      ast_to_arg(&cond, prog, fn, sp, &op.jmp.cond);
      ast_del(&cond);
      da_append(&fn->fn_body, op);
    }
    size_t jmp_else = fn->fn_body.count - 1;
    // if branch
    if (!compile_stat(l, prog, fn, sp)) return false;

    if (l->current.kind == TOKEN_ELSE) {
      da_append(&fn->fn_body, (Op) { .kind = OP_JMP });
      size_t jmp_end = fn->fn_body.count - 1;

      da_append(&fn->fn_body, (Op) { .kind = OP_LABEL });
      size_t else_label = fn->fn_body.count - 1;
      fn->fn_body.items[jmp_else].jmp.label = else_label;

      // else branch
      if (!prefetch_not_none(l)) return false;
      if (!compile_stat(l, prog, fn, sp)) return false;

      da_append(&fn->fn_body, (Op) { .kind = OP_LABEL });
      size_t end_label = fn->fn_body.count - 1;
      fn->fn_body.items[jmp_end].jmp.label = end_label;
    } else {
      da_append(&fn->fn_body, (Op) { .kind = OP_LABEL });
      size_t end_label = fn->fn_body.count - 1;
      fn->fn_body.items[jmp_else].jmp.label = end_label;
    }
  } else {
    if (!compile_stat_simple(l, prog, fn, sp)) return false;
  }

  // a simple statment can be followed with an optional ';'
  // This makes "if true foo(); else bar();" acceptable
  // because 'foo();' is a single statement
  // insteed of a function call followed by a empty statement.
  // "if true ;; else foo()" is not acceptable
  // because both ';' are two empty statement
  // because they are not followed by a simple statement
  if (l->current.kind == ';') {
    if (!prefetch_not_none(l)) return false;
  }

  return true;
}

static bool compile_def(Lexer *l, Program *prog, Scoop *sp);

static bool compile_block(Lexer *l, Program* prog, Fn *fn, Scoop *sp)
{
  assert(l->current.kind == '{');
  
  Scoop *block = alloc_scoop(&prog->symbols, sp);
  if (!prefetch_not_none(l)) return false;
  while (l->current.kind != '}') {
    if (l->current.kind == TOKEN_VAR) {
      if (!compile_local_var(l, prog, fn, block)) return false;
      continue;
    }

    if (compile_def(l, prog, block)) {
      continue;
    }

    if (!compile_stat(l, prog, fn, block)) return false;
  }
  lexer_next(l);
  return true;
}

static bool compile_fn_sign(Lexer *l, Fn *fn, Program *prog, Scoop *sp)
{
  assert(l->current.kind == TOKEN_FN);

  if (!prefetch_expect_token(l, TOKEN_ID)) return false;
  if (!insert_sym(sp, l->current, SYMBOL_FN, fn)) return false;

  *fn = (Fn) {
    .loc = l->current.start,
    .type = {.kind = TYPE_FN},
    .local = alloc_scoop(&prog->symbols, prog->global),
  };

  if (!prefetch_expect_token(l, '(')) return false;
  if (!prefetch_expect_tokens(l, ')', TOKEN_ID)) return false;

  while (l->current.kind != ')') {
    // TODO: it leaks;
    assert(l->current.kind == TOKEN_ID);
    
    Var *var = calloc(1, sizeof(Var));
    da_append(&fn->args, var);
    var->loc = l->current.start;
    if (!insert_sym(fn->local, l->current, SYMBOL_VAR, var)) return false;
    
    if (!prefetch_expect_token(l, ':')) return false;
    if (!lexer_next(l)) return false;
    if (!compile_type_expr(l, &var->type)) return false;

    da_append(&fn->type.fn_type.arg_types, (type_clone(var->type)));

    if (!prefetch_expect_tokens(l, ',', ')')) return false;

    if (l->current.kind == ',') {
      if (!prefetch_expect_token(l, TOKEN_ID)) return false;
    }
  }
  assert(l->current.kind == ')');

  if (!prefetch_expect_token(l, ':')) return false;
  if (!prefetch_not_none(l)) return false;

  TypeExpr ret_type = {0};
  if (!compile_type_expr(l, &ret_type)) return false;
  fn->type.fn_type.ret_type = malloc(sizeof(TypeExpr));
  *fn->type.fn_type.ret_type = ret_type;

  return true;
}

static bool compile_function(Lexer *l, Program *prog, Scoop *sp)
{
  Fn *fn = calloc(1, sizeof(Fn));
  da_append(&prog->fn_list, fn);

  if (!compile_fn_sign(l, fn, prog, sp)) return false;

  if (!prefetch_expect_token(l, '{')) return false;
  return compile_block(l, prog, fn, fn->local);
}

static size_t search_ptr(void *arr, size_t n, void *val)
{
  void **p = arr;
  for (size_t i = 0; i < n; ++i) {
    if (val == p[i]) return i;
  }
  return n;
}

static bool backpatch_name_arg_impl(Arg *arg, Program *prog, Fn *fn)
{
  if (arg->kind != ARG_NAME) return true;

  SymSearchResult r = sym_search(arg->scoop, arg->name);
  if (r.scoop == NULL) {
    pcompile_info(arg->loc,
                  "error: symbol `"SV_Fmt"` refered but never defined\n",
                  SV_Arg(arg->name));
    return false;
  }
  
  static_assert(__symbol_kind_count == 3);
  switch (r.sym->kind) {
  case SYMBOL_FN:
    arg->label = search_ptr(prog->fn_list.items, prog->fn_list.count, r.sym->ptr);
    arg->kind = ARG_FN;
    assert(arg->label < prog->fn_list.count);
    arg->type = type_clone(prog->fn_list.items[arg->label]->type);
    return true;
  break;
  case SYMBOL_VAR: {
    Var *var = r.sym->ptr;
    Scoop *s = r.scoop;
    while (s != NULL && s != prog->global && var->loc.row >= arg->loc.row) {
      SymSearchResult r = sym_search(s->upper, arg->name);
      var = r.sym->ptr;
      s = r.scoop;
    }
    if (r.scoop == NULL) {
      pcompile_info(arg->loc,
                    "error: local variable `"SV_Fmt"` refered before it is defined\n",
                    SV_Arg(arg->name));
      pcompile_info(r.sym->var->loc,
                    "info: it is defined here\n");
      return false;
    }
    
    if (s == prog->global) {
      TODO("support global variables.");
    }

    arg->label = search_ptr(fn->vars.items, fn->vars.count, var);
    if (arg->label < fn->vars.count) {
      arg->kind = ARG_VAR_LOC;
      arg->type = type_clone(fn->vars.items[arg->label]->type);
    } else {
      arg->label = search_ptr(fn->args.items, fn->args.count, var);
      arg->kind = ARG_VAR_ARG;
      assert(arg->label < fn->args.count);
      arg->type = type_clone(fn->args.items[arg->label]->type);
    }
    return true;
  }
  case SYMBOL_EXTERN:
    arg->label = search_ptr(prog->externs.items, prog->externs.count, r.sym->ptr);
    arg->kind = ARG_EXTERN;
    assert(arg->label < prog->externs.count);
    arg->type = type_clone(prog->externs.items[arg->label]->type);
    return true;
  default: UNREACHABLE("");
  }
}

static bool backpatch_name_args(Program *prog)
{
  bool success = true;
  da_foreach (Fn*, fn_ptr, &prog->fn_list) {
    Fn *fn = *fn_ptr;
    da_foreach (Op, op, &fn->fn_body) {
      static_assert(__op_kind_count == 7);
      switch (op->kind) {
      case OP_RETURN:
        success = backpatch_name_arg_impl(&op->ret_val, prog, fn) && success;
        break;
      case OP_SET_VAR:
        success = backpatch_name_arg_impl(&op->set_var.val, prog, fn) && success;
        success = backpatch_name_arg_impl(&op->set_var.var, prog, fn) && success;
        break;
      case OP_INVOKE:
        success = backpatch_name_arg_impl(&op->invoke.fn, prog, fn) && success;
        da_foreach (Arg, arg, &op->invoke.args) {
          success = backpatch_name_arg_impl(arg, prog, fn) && success;
        }
        break;
      case OP_BINOP:
        success = backpatch_name_arg_impl(&op->binop.lhs, prog, fn) && success;
        success = backpatch_name_arg_impl(&op->binop.rhs, prog, fn) && success;
        break;
      case OP_JMP_ELSE:
        success = backpatch_name_arg_impl(&op->jmp.cond, prog, fn) && success;
        break;
      case OP_LABEL:
      case OP_JMP:
        // nothing to do because these op has no argument.
        break;
     default: UNREACHABLE("op");
      }
    }
  }

  return success;
}

static bool detect_arg_type(Arg *arg, Program *prog, Fn *fn) {
  if (arg->type.kind != TYPE_UNKNOWN) return true;
  if (arg->kind == ARG_NONE) return true;

  assert(arg->kind != ARG_NAME);

  ExternList *externs = &prog->externs;
  FnList *fn_list = &prog->fn_list;
  VarList *vars = &fn->vars;
  VarList *args = &fn->args;

  TypeExpr *type = NULL;

  static_assert(__arg_kind_count == 8);
  switch(arg->kind) {
  case ARG_EXTERN:
    type = &externs->items[arg->label]->type;
    break;
  case ARG_FN:
    type = &fn_list->items[arg->label]->type;
    break;
  case ARG_VAR_LOC:
    type = &vars->items[arg->label]->type;
    break;
  case ARG_VAR_ARG:
    type = &args->items[arg->label]->type;
    break;
  default:
    UNREACHABLE("detect_arg_type");
  }

  assert(type != NULL);
  assert(type->kind != TYPE_UNKNOWN);
  arg->type = type_clone(*type);
  return true;
}

static bool detect_var_type(Arg *var, Arg *val, Program *prog, Fn *fn)
{
  UNUSED(prog);
  assert(val->type.kind != TYPE_UNKNOWN);
  if (var->type.kind != TYPE_UNKNOWN) return true;

  VarList *vars = &fn->vars;
  VarList *args = &fn->args;

  TypeExpr *var_type = NULL;
  // not every arg type can occur in here
  static_assert(__arg_kind_count == 8);
  switch (var->kind) {
  case ARG_VAR_LOC:
    assert(var->label < vars->count);
    var_type = &vars->items[var->label]->type;
    break;
  case ARG_VAR_ARG:
    assert(var->label < args->count);
    var_type = &args->items[var->label]->type;
    break;
  case ARG_NAME:
    UNREACHABLE("currently this is imposible");
  default: UNREACHABLE("fix_type_var");
  }

  if (var_type->kind == TYPE_UNKNOWN) {
    *var_type = type_clone(val->type);
  }

  var->type = type_clone(val->type);
  return true;
}

static bool detect_binop_dst_type(VarList *vars, OpBinop *binop)
{
  Arg *lhs = &binop->lhs;
  Arg *rhs = &binop->rhs;
  Arg *dst = &binop->dst;

  assert(lhs->type.kind != TYPE_UNKNOWN);
  assert(rhs->type.kind != TYPE_UNKNOWN);
  assert(dst->kind == ARG_VAR_LOC);

  static_assert(__binop_kind_count == 11);
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

    vars->items[dst->label]->type = type_bool();
    
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
    vars->items[dst->label]->type = type_clone(lhs->type);

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
      static_assert(__op_kind_count == 7);
      switch (op->kind) {
      case OP_RETURN:
        if (!detect_arg_type(&op->ret_val, prog, fn)) return false;
        break;
      case OP_SET_VAR:
        if (!detect_arg_type(&op->set_var.val, prog, fn)) return false;
        if (!detect_var_type(&op->set_var.var, &op->set_var.val, prog, fn)) return false;
        break;
      case OP_INVOKE: {
        if (!detect_arg_type(&op->invoke.fn, prog, fn)) return false;
        da_foreach (Arg, arg, &op->invoke.args) {
          if (!detect_arg_type(arg, prog, fn)) return false;
        }

        if (!op->invoke.ret_ignore) {
          Var *ret = fn->vars.items[op->invoke.result_label];
          TypeExpr *ret_type = fn->type.fn_type.ret_type;

          assert(ret_type->kind != TYPE_UNKNOWN);
          ret->type = type_clone(*ret_type);
        }
      } break;
      case OP_BINOP:
        if (!detect_arg_type(&op->binop.lhs, prog, fn)) return false;
        if (!detect_arg_type(&op->binop.rhs, prog, fn)) return false;
        if (!detect_binop_dst_type(&fn->vars, &op->binop)) return false;
        break;
      case OP_JMP_ELSE:
        if (!detect_arg_type(&op->jmp.cond, prog, fn)) return false;
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

static bool compile_def(Lexer *l, Program *prog, Scoop *sp)
{
  if (l->current.kind == TOKEN_FN) {
    return compile_function(l, prog, sp);
  } else if (l->current.kind == TOKEN_EXT) {
    if (!prefetch_expect_token(l, TOKEN_ID)) return false;

    Extern *ext = calloc(1, sizeof(Extern));
    *ext = (Extern) {
      .linkname = l->current.str,
      .loc = l->current.start,
    };
    da_append(&prog->externs, ext);
    if (!insert_sym(sp, l->current, SYMBOL_EXTERN, ext)) return false;

    if (!prefetch_expect_token(l, ':')) return false;
    if (!prefetch_not_none(l)) return false;
    if (!compile_type_expr(l, &ext->type)) return false;

    lexer_next(l);
    if (l->current.kind == ';') lexer_next(l);
    
    return true;
  }
  
  return false;
}

static bool compile_file(Lexer *l, Program *prog)
{
  lexer_next(l);
  while (l->current.kind != TOKEN_EOF && l->current.kind != TOKEN_ERR) {
    Cursor loc = l->current.start;
    if (!compile_def(l, prog, prog->global)) {
      Cursor c = l->current.start;
      if (loc.row == c.row && loc.col == c.col)
      pcompile_info(loc,
                    "error: expected a defination in global scoop.\n");
      return false;
    }
  }

  return l->current.kind == TOKEN_EOF;
}

static bool check_type(Program *prog)
{
  da_foreach (Fn *, fn_ptr, &prog->fn_list) {
    Fn *fn = *fn_ptr;
    TypeExpr *fn_type = &fn->type;

    assert(fn_type->kind == TYPE_FN);
    da_foreach (Op, op, &fn->fn_body) {
      static_assert(__op_kind_count == 7);
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
          pcompile_info(op->loc, "error: incompatible types when assigning to type \"");
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
      static_assert(__op_kind_count == 7);
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

bool compile_program(Lexer *l, Program *prog)
{
  prog->global = alloc_scoop(&prog->symbols, NULL);
  if (!compile_file(l, prog)) return false;
  if (!backpatch_name_args(prog)) return false;
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

  free_all_symbol(&prog->symbols);
}

