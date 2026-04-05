#include "parser.h"
#include "lexer.h"

#include "nob.h"
#include "utils.h"

size_t alloc_var(VarList *vars)
{
  Var var = (Var) {
    .type = {.kind = TYPE_UNKNOWN}
  };
  da_append(vars, var);
  return vars->count - 1;
}

static void destroy_type_expr(TypeExpr* type)
{
  if (type == NULL) return;
  
  switch(type->kind) {
  case TYPE_INT: break;
  case TYPE_UINT: break;
  case TYPE_VOID: break;
  case TYPE_UNKNOWN: break;
  case TYPE_PTR:
    assert(type->ref_type != NULL);
    destroy_type_expr(type->ref_type);
    free(type->ref_type);
    break;
  case TYPE_FN: {
    if (type->fn_type.ret_type != NULL) {
      destroy_type_expr(type->fn_type.ret_type);
      free(type->fn_type.ret_type);
    }
    
    da_foreach (TypeExpr, arg, &type->fn_type.arg_types) {
      destroy_type_expr(arg);
    }
    if (type->fn_type.arg_types.count > 0) da_free(type->fn_type.arg_types);
  } break;
  default: UNREACHABLE("type");
  }
}

static void destroy_op(Op *op)
{
  switch (op->kind) {
  case OP_INVOKE:
    da_free(op->invoke.args);
    break;
  case OP_RETURN: case OP_SET_VAR:
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

  if (fn->local.count != 0) {
    da_foreach (Var, var, &fn->local) {
      destroy_type_expr(&var->type);
    }
    da_free(fn->local);
  }

  if (fn->args.count != 0) {
    da_foreach (Var, var, &fn->args) {
      destroy_type_expr(&var->type);
    }
    da_free(fn->args);
  }
}

static void destroy_fn_list(FnList *fn_list)
{
  da_foreach (Fn, fn, fn_list) {
    destroy_fn(fn);
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

static TypeExpr ptr_type(TypeExpr inner)
{
  TypeExpr type = {
    .kind = TYPE_PTR,
    .size = 8,
    .ref_type = malloc(sizeof(TypeExpr))
  };
  *type.ref_type = inner;
  
  return type;
}

static TypeExpr fn_type(TypeExpr ret_type, TypeList arg_types, bool va_args)
{
  TypeExpr type = {
    .kind = TYPE_FN,
    .size = 8,
    .fn_type = {
      .ret_type = malloc(sizeof(TypeExpr)),
      .arg_types = arg_types,
      .va_args = va_args,
    },
  };
  *type.ref_type = ret_type;
  
  return type;
}

static TypeExpr type_clone(TypeExpr t)
{
  TypeExpr result = {0};
  switch (t.kind) {
  case TYPE_UNKNOWN:
  case TYPE_VOID:
  case TYPE_INT:
  case TYPE_UINT:
    result = t;
    break;
  case TYPE_PTR:
    result = ptr_type(type_clone(*t.ref_type));
    break;
  case TYPE_FN: {
    TypeList arg_types = {0};
    da_foreach (TypeExpr, arg, &t.fn_type.arg_types) {
      da_append(&arg_types, type_clone(*arg));
    }
    result = fn_type(*t.fn_type.ret_type, arg_types, t.fn_type.va_args);
  }break;
  default: UNREACHABLE("type_clone");
  }

  return result;
}

static Arg arg_local_var(Fn *fn, size_t i)
{
  assert(i < fn->local.count);
  return (Arg) {
    .kind = ARG_VAR_LOC,
    .type = type_clone(fn->local.items[i].type),
    .label = i,
  };
}

static bool compile_arg(Lexer *l, Program *prog, Arg *arg)
{
  assert(l->current.kind != TOKEN_EOF);
  
  switch (l->current.kind) {
  case TOKEN_ID:
    arg->kind = ARG_NAME;
    arg->name = l->current.token;
    arg->loc = l->current.start;
    arg->type = (TypeExpr){.kind = TYPE_UNKNOWN};

    break;
  case TOKEN_STR:
    arg->kind = ARG_LIT_STR;
    arg->label = compile_strlit(prog, l->current.token);
    arg->type = ptr_type((TypeExpr) {
        .kind = TYPE_INT,
        .size = 1,
      });
    break;
  case TOKEN_INT:
    arg->kind = ARG_LIT_INT;
    arg->num_int = sv_to_int(l->current.token);
    arg->type = (TypeExpr) {
      .kind = TYPE_INT,
      .size = 4,
    };
    break;
  default:
    size_t mark = temp_save(); {
      pcompile_info(l->current.start, "error: invalid token ");
      dump_token_kind(stderr, l->current.kind);
      fprintf(stderr, " in an argument\n");
    } temp_rewind(mark);
    return false;
  }

  return true;
}

static bool compile_expr(Lexer *l, Program *prog, Fn *fn, Arg *exp_result);

static bool compile_invoke_args(Lexer *l, Program *prog, Fn *fn, ArgList *args)
{
  Arg arg = {0};
  if (!prefetch_not_none(l)) return false;

  while (l->current.kind != ')') {
    if (!compile_expr(l, prog, fn, &arg)) return false;
    da_append(args, arg);
        
    if (l->current.kind != ',' && l->current.kind != ')') {
      pcompile_info(l->current.start, "error: expected ',' or ')', but got ");
      dump_token_kind(stderr, l->current.kind);
      fprintf(stderr, "\n");
      free(args->items);
      return false;
    }

    if (l->current.kind == ',') {
      if (!prefetch_not_none(l)) return false;
    }
  }
  if (!prefetch_not_none(l)) return false;
  return true;
}

static bool compile_expr(Lexer *l, Program *prog, Fn *fn, Arg *exp_result)
{
  Arg arg = {0};
  
  Cursor loc = l->current.start;
  if (!compile_arg(l, prog, &arg)) return false;

  if (!prefetch_not_none(l)) return false;
  if (l->current.kind == '(') {
    Op op = {
      .kind = OP_INVOKE,
      .loc = loc,
      .invoke = {
        .fn = arg,
        .result_label = alloc_var(&fn->local),
      },
    };
    if (!compile_invoke_args(l, prog, fn, &op.invoke.args)) return false;
    da_append(&fn->fn_body, op);

    *exp_result = (Arg) {
      .kind = ARG_VAR_LOC,
      .type = {.kind = TYPE_UNKNOWN},
      .label = op.invoke.result_label,
    };
  } else {
    *exp_result = arg;
  }

  return true;
}

static bool compile_statement(Lexer *l, Program *prog, Fn *fn)
{
  Arg arg;
  Cursor loc = l->current.start;
  if (!compile_expr(l, prog, fn, &arg)) return false;

  if (l->current.kind == '=') {
    Arg val;
    if (!prefetch_not_none(l)) return false;
    if (!compile_expr(l, prog, fn, &val)) return false;
    
    Op set_var = {
      .kind = OP_SET_VAR,
      .loc = loc,
      .set_var = {
        .var = arg,
        .val = val,
      },
    };
    da_append(&fn->fn_body, set_var);
  }
  
  return true;
}

static_assert(TYPE_KIND_COUNT == 6);
static struct {
  int token;
  TypeExpr type;
} internal_types[] = {
  {
    .token = TOKEN_VOID,
    .type = { .kind = TYPE_VOID, .size = 0, },
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
    *type = ptr_type(ref_type);
    return true;
  }
  default:
    pcompile_info(l->current.start, "error: expected a type but got ");
    dump_token_kind(stderr, l->current.kind);
    fprintf(stderr, "\n");
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
  if (!compile_type_expr(l, type->fn_type.ret_type)) return_defer(false);

  return_defer(true);

 defer:
  if (!result) destroy_type_expr(type);
  return result;
}

static bool compile_local_var_singn(Lexer *l, Fn *fn)
{
  bool result;

  Var var = {0};
  if (!prefetch_expect_token(l, TOKEN_ID)) return_defer(false);

  var.name = l->current.token;
  var.loc = l->current.start;
  if (!prefetch_not_none(l)) return_defer(false);

  if (l->current.kind == ':') {
    if (!prefetch_not_none(l)) return_defer(false);
    if (!compile_type_expr(l, &var.type)) return_defer(false);
    if (var.type.kind == TYPE_VOID) {
      pcompile_info(l->current.start, "error: the type of a local variable cannot be \"void\"");
      return_defer(false);
    }
    if (!prefetch_not_none(l)) return_defer(false);
  }
  
  da_append(&fn->local, var);
  return_defer(true);
 defer:
  if (!result) destroy_type_expr(&var.type);
  return result;
}

static bool compile_local_var(Lexer *l, Program *prog, Fn *fn)
{
  if (!compile_local_var_singn(l, fn)) return false;
  size_t pos = fn->local.count - 1;
  
  if (l->current.kind != '=') return true;

  Cursor loc = l->current.start;
  if (!prefetch_not_none(l)) return false;
  Arg val = {0};
  if (!compile_expr(l, prog, fn, &val)) return false;    
  Op set_var = {
    .kind = OP_SET_VAR,
    .loc = loc,
    .set_var = {
      .var = arg_local_var(fn, pos),
      .val = val,
    },
  };
  da_append(&fn->fn_body, set_var);

  return true;
}

static bool compile_fn_body(Lexer *l, Program *prog, Fn *fn) {
  assert(l->current.kind == '{');

  bool returned = false;
  if (!prefetch_not_none(l)) return false;
  while (l->current.kind != '}') {
    if (l->current.kind == ';') {
      if (!prefetch_not_none(l)) return false;
    } else if (l->current.kind == TOKEN_RET) {
      Op ret = {
        .kind = OP_RETURN,
        .loc = l->current.start,
      };
      if (!prefetch_not_none(l)) return false;
      if (!compile_expr(l, prog, fn, &ret.ret_val)) return false;
      da_append(&fn->fn_body, ret);
      returned = true;
    } else if (l->current.kind == TOKEN_VAR) {
      if (!compile_local_var(l, prog, fn)) return false;
    } else {
      if (!compile_statement(l, prog, fn)) return false;
    }
  }

  if (!returned) {
    Op ret = {
      .kind = OP_RETURN,
      .loc = l->current.start,
      .ret_val = {
        .kind = ARG_NONE,
        .type = { .kind = TYPE_VOID },
      },
    };
    da_append(&fn->fn_body, ret);
  }

  return true;
}

static bool compile_fn_sign(Lexer *l, Fn *fn)
{
  assert(l->current.kind == TOKEN_FN);

  if (!prefetch_expect_token(l, TOKEN_ID)) return false;

  *fn = (Fn) {
    .name = l->current.token,
    .loc = l->current.start,
    .type = {.kind = TYPE_FN},
  };

  if (!prefetch_expect_token(l, '(')) return false;
  if (!prefetch_expect_tokens(l, ')', TOKEN_ID)) return false;
  
  bool result;
  while (l->current.kind != ')') {
    Var var = {0};
    assert(l->current.kind == TOKEN_ID);
    var.name = l->current.token;
    var.loc = l->current.start;
    
    if (!prefetch_expect_token(l, ':')) return_defer(false);
    if (!lexer_next(l)) return_defer(false);
    if (!compile_type_expr(l, &var.type)) return_defer(false);
    
    da_append(&fn->args, var);
    da_append(&fn->type.fn_type.arg_types, (type_clone(var.type)));
    
    if (!prefetch_expect_tokens(l, ',', ')')) return_defer(false);

    if (l->current.kind == ',') {
      if (!prefetch_expect_token(l, TOKEN_ID)) return_defer(false);
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
  
 defer:
  destroy_fn(fn);
  return result;
}

static bool compile_function(Lexer *l, Program *prog)
{
  bool result;
  
  Fn fn = {0};
  if (!compile_fn_sign(l, &fn)) return_defer(false);

  if (!prefetch_expect_token(l, '{')) return_defer(false);
  if (!compile_fn_body(l, prog, &fn)) return_defer(false);
  if (!expect_token(l, '}')) return_defer(false);

  da_append(&prog->fn_list, fn);
  return true;
 defer:
  destroy_fn(&fn);
  return result;
}

static bool backpatch_name_arg_impl(Arg *arg, Program *prog, Fn *fn)
{  
  if (arg->kind != ARG_NAME) return true;

  size_t i = dct_geti(&fn->args, arg->name);
  if (i < fn->args.count) {
    *arg = (Arg) {
      .kind = ARG_VAR_ARG,
      .label = i,
    };
    return true;
  }
  
  i = dct_geti(&fn->local, arg->name);
  if (i < fn->local.count) {
    Cursor use_loc = arg->loc;
    Cursor def_loc = label_item(&fn->local, i).loc;
    if (use_loc.row <= def_loc.row) {
      pcompile_info(use_loc, "error: local variable `"SV_Fmt"` is used before its defination.\n", SV_Arg(arg->name));
      pcompile_info(def_loc, "info: it is defined here.\n");
      return false;
    }
    
    *arg = (Arg) {
      .kind = ARG_VAR_LOC,
      .label = i,
    };
    return true;
  }
  
  i = dct_geti(&prog->externs, arg->name);
  if (i < prog->externs.count) {
    *arg = (Arg) {
      .kind = ARG_EXTERN,
      .label = i,
    };
    return true;
  }

  i = dct_geti(&prog->fn_list, arg->name);
  if (i < prog->fn_list.count) {
    *arg = (Arg) {
      .kind = ARG_FN,
      .label = i,
    };
    return true;
  }
  
  pcompile_info(arg->loc,
                "error: refered symbol \""SV_Fmt"\" is not defined\n", SV_Arg(arg->name));
  return false;
}

static bool backpatch_name_args(Program *prog)
{
  bool success = true;
  da_foreach (Fn, fn, &prog->fn_list) {
    da_foreach (Op, op, &fn->fn_body) {
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
  VarList *local = &fn->local;
  VarList *args = &fn->args;

  TypeExpr * type = NULL;
  switch(arg->kind) {
  case ARG_EXTERN:
    type = &label_item(externs, arg->label).type;
    break;
  case ARG_FN:
    type = &label_item(fn_list, arg->label).type;
    break;
  case ARG_VAR_LOC:
    type = &label_item(local, arg->label).type;
    break;
  case ARG_VAR_ARG:
    type = &label_item(args, arg->label).type;
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

  VarList *local = &fn->local;
  VarList *args = &fn->args;
  
  TypeExpr *var_type = NULL;
  if (var->kind == ARG_VAR_LOC) {
    assert(var->label < local->count);
    var_type = &local->items[var->label].type;
  } else if (var->kind == ARG_VAR_ARG) {
    assert(var->label < args->count);
    var_type = &args->items[var->label].type;
  } else if (var->kind == ARG_NAME) {
    TODO("");
  } else {
    UNREACHABLE("fix_type_var");
  }

  if (var_type->kind == TYPE_UNKNOWN) {
    *var_type = type_clone(val->type);
  }

  var->type = type_clone(val->type);
  return true;
}

static bool detect_all_unknown_type(Program *prog)
{
  da_foreach (Fn, fn, &prog->fn_list) {
    da_foreach (Op, op, &fn->fn_body) {
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
        
        Var *ret = &label_item(&fn->local, op->invoke.result_label);
        TypeExpr *ret_type = fn->type.fn_type.ret_type;

        assert(ret_type->kind != TYPE_UNKNOWN);
        ret->type = type_clone(*ret_type);
      } break;
      default: UNREACHABLE("op");
      }
    }
  }
  return true;
}

static bool compile_file(Lexer *l, Program *prog)
{
  while (lexer_next(l)) {
    if (l->current.kind == ';') {
      continue;
    }
    
    if (l->current.kind == TOKEN_FN) {
      if (!compile_function(l, prog)) return false;
    } else if (l->current.kind == TOKEN_EXT) {
      if (!prefetch_expect_token(l, TOKEN_ID)) return false;
      
      Extern ext = {
        .name = l->current.token,
        .loc = l->current.start,
      };
      
      if (!prefetch_expect_token(l, ':')) return false;
      if (!prefetch_not_none(l)) return false;
      if (!compile_type_expr(l, &ext.type)) return false;

      da_append(&prog->externs, ext);
    } else {
      UNREACHABLE("");
    }
  }

  return true;
}

static bool type_matched(TypeExpr *required, TypeExpr *actual)
{
  // TODO: check pointer type and function type
  if (required->kind != actual->kind) {
    return false;
  }

  if (required->kind == TYPE_INT || required->kind == TYPE_UINT) {
    return required->size >= actual->size;
  }

  return true;
}

void dump_type_expr(TypeExpr *type, FILE *stream)
{
  UNUSED(stream);
  switch(type->kind) {
  case TYPE_UNKNOWN:
    fprintf(stream, "unknown type");
    break;
  case TYPE_VOID:
    fprintf(stream, "void");
    break;
  case TYPE_INT:
    fprintf(stream, "i%ld", type->size * 8);
    break;
  case TYPE_UINT:
    fprintf(stream, "u%ld", type->size * 8);
    break;
  case TYPE_FN: 
    fprintf(stream, "fn(");
    for (size_t i = 0; i < type->fn_type.arg_types.count; ++i) {
      dump_type_expr(&type->fn_type.arg_types.items[i], stream);
      if (i + 1 < type->fn_type.arg_types.count) {
        fprintf(stream, ",");
      } else if (type->fn_type.va_args) {
        fprintf(stream, ",...");
      }
    }
    fprintf(stream, "):");
    dump_type_expr(type->fn_type.ret_type, stream);
    break;
  case TYPE_PTR:
    fprintf(stream, "*");
    dump_type_expr(type->ref_type, stream);
    break;
  default: UNREACHABLE("type");
  }
}

static bool check_type(Program *prog)
{
  da_foreach (Fn, fn, &prog->fn_list) {
    TypeExpr *fn_type = &fn->type;
    
    assert(fn_type->kind == TYPE_FN);
    da_foreach (Op, op, &fn->fn_body) {
      switch (op->kind) {
      case OP_RETURN:
        TypeExpr *ret_type = &op->ret_val.type;
        if (!type_matched(fn_type->fn_type.ret_type, ret_type)) {
          pcompile_info(op->loc, "error: the return type of "SV_Fmt" is required to be ", SV_Arg(fn->name));
          dump_type_expr(fn_type->fn_type.ret_type, stderr);
          fprintf(stderr, ", but got ");
          dump_type_expr(ret_type, stderr);
          fputc('\n', stderr);
          return false;
        }
        break;
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
      case OP_INVOKE:
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

#define check_redefined(list, defs) do {                                \
    for (size_t i = 0; i < (list)->count; ++i) {                        \
      String_View name = (list)->items[i].name;                         \
      Cursor loc = (list)->items[i].loc;                                \
      if (sv_eq(name, sv_from_cstr(""))) continue;                      \
      size_t j = dct_geti((defs), name);                                \
      if (j < (defs)->count) {                                          \
        Cursor first_loc = (defs)->items[j].loc;                        \
        pcompile_info(loc, "error: symbol `"SV_Fmt"` redefined\n", SV_Arg(name)); \
        pcompile_info(first_loc, "info: it is firstly defined here\n"); \
        result = false;                                                 \
      } else {                                                          \
        da_append((defs), ((Def) {.name = name, .loc = loc}));          \
      }                                                                 \
    }                                                                   \
  } while(0);

static bool check_symbol_redefined(Program *prog)
{
  bool result = true;
  
  DefList defs = {0};
  check_redefined(&prog->externs, &defs);
  check_redefined(&prog->fn_list, &defs);

  da_foreach (Fn, fn, &prog->fn_list) {
    defs.count = 0;
    check_redefined(&fn->args, &defs);
    check_redefined(&fn->local, &defs);
  }

  da_free(defs);
  
  return result;
}

bool compile_program(Lexer *l, Program *prog)
{
  if (!compile_file(l, prog)) return false;
  if (!check_symbol_redefined(prog)) return false;
  if (!backpatch_name_args(prog)) return false;
  if (!detect_all_unknown_type(prog)) return false;
  if (!check_type(prog)) return false;
  
  return true;
}

void destroy_program(Program *prog)
{
  da_foreach (Extern, ext, &prog->externs) {
    destroy_type_expr(&ext->type);
  }
  da_free(prog->externs);

  destroy_fn_list(&prog->fn_list);
  if (prog->str_lits.capacity > 0) {
    da_free(prog->str_lits);
  }
}

