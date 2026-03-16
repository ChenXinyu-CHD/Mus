#include "parser.h"
#include "lexer.h"

#include "nob.h"

static void vpcompile_info(Cursor cs, const char* fmt, va_list args)
{
  size_t mark = temp_save(); {
    va_list ap;
    va_copy(ap, args);
    char *msg = temp_vsprintf(fmt, ap);
    va_end(ap);
  
    fprintf(stderr, CS_Fmt" %s", CS_Arg(cs), msg);
  } temp_rewind(mark);
}

static void pcompile_info(Cursor cs, const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vpcompile_info(cs, fmt, args);
  va_end(args);
}

static bool next_token(Lexer *l)
{
  bool result = lexer_next(l);
  if (l->current.kind == TOKEN_ERR) {
    pcompile_info(l->current.start, "error: parse error when read \""SV_Fmt"\"\n",
                  SV_Arg(l->current.token));
  }

  return result;
}

static bool prefetch_not_none(Lexer *l)
{
  bool result = next_token(l);
  if (l->current.kind == TOKEN_EOF) {
    pcompile_info(l->current.start, "error: unexpected EOF\n");
    return false;
  }
  
  return result;
}

static bool expect_token(Lexer *l, int token)
{
  if (l->current.kind == token) return true;
  
  pcompile_info(l->current.start, "error: expect token ");
  dump_token_kind(stderr, token);
  fprintf(stderr, ", but got");
  dump_token_kind(stderr, token);
  fprintf(stderr, "\n");
  
  return false;
}

static bool prefetch_expect_token(Lexer *l, int token)
{
  next_token(l);
  if (!expect_token(l, token)) return false;
  return true;
}

static bool prefetch_expect_tokens_(Lexer *l, ...)
{
  next_token(l);
  bool found = false;
  
  va_list ap; va_start(ap, l); {
    int arg = va_arg(ap, int);
    while (arg != TOKEN_ERR) {
      if (arg == l->current.kind) {
        found = true;
        break;
      }
      arg = va_arg(ap, int);
    }
  } va_end(ap);

  if (found) return true;

  va_start(ap, l); {
    pcompile_info(l->current.start, "error: expect token ");
    
    int arg = va_arg(ap, int);
    while (arg != TOKEN_ERR) {
      dump_token_kind(stderr, arg);
      fprintf(stderr, ", ");
      arg = va_arg(ap, int);
    }
    fprintf(stderr, "but got");
    dump_token_kind(stderr, l->current.kind);
    fprintf(stderr, "\n");
  } va_end(ap);
  
  return false;
}

#define prefetch_expect_tokens(l, ...)                        \
  prefetch_expect_tokens_(l, __VA_ARGS__, TOKEN_ERR)

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
    assert(type->fn_type.ret_type != NULL);
    destroy_type_expr(type->fn_type.ret_type);
    free(type->fn_type.ret_type);
    
    da_foreach (TypeExpr, arg, &type->fn_type.arg_types) {
      destroy_type_expr(arg);
    }
    if (type->fn_type.arg_types.count > 0) da_free(type->fn_type.arg_types);
  } break;
  default: UNREACHABLE("type");
  }
}

static void destroy_symbol(Symbol *sym)
{
  destroy_type_expr(&sym->type);
}

static void destroy_arg(Arg *arg)
{
  destroy_type_expr(&arg->type);
  switch(arg->kind) {
  case ARG_NONE: break;
  case ARG_NAME: break;
  case ARG_VAR_LOC: break;
  case ARG_LIT_INT: break;
  case ARG_LIT_STR: break;
  default: UNREACHABLE("destroy arg");
  }
}

static void destroy_op(Op *op)
{
  switch (op->kind) {
  case OP_INVOKE:
    destroy_arg(&op->invoke.fn);
    da_foreach(Arg, arg, &op->invoke.args) {
      destroy_arg(arg);
    }
    break;
  case OP_RETURN: destroy_arg(&op->ret_val); break;
  case OP_SET_VAR:
    destroy_arg(&op->set_var.var);
    destroy_arg(&op->set_var.val);
    break;
  default: UNREACHABLE("destroy op");
  }
}

static void destroy_fn_list(FnList *fn_list)
{
  da_foreach (Fn, fn, fn_list) {
    da_foreach (Op, op, &fn->fn_body) {
      destroy_op(op);
    }
    da_free(fn->fn_body);
    
    da_foreach (Symbol, sym, &fn->local) {
      destroy_symbol(sym);
    }
    da_free(fn->local);
  }
  da_free(*fn_list);
}

#define expect_ids(l, filename, ...)            \
  expect_ids_(l, filename, __VA_ARGS__, NULL)

static size_t search_symbol(SymbolList *syms, String_View name)
{
  for (size_t i = 0; i < syms->count; ++i) {
    if (sv_eq(syms->items[i].name, name)) {
      return i;
    }
  }
  
  return syms->count;
}

static bool symbol_defined(SymbolList *syms, String_View name)
{
  return search_symbol(syms, name) < syms->count;
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
    .label = i + 1.
  };
}

static bool compile_arg(Lexer *l, Program *prog, Fn *fn, Arg *arg)
{
  UNUSED(prog);
  assert(l->current.kind != TOKEN_EOF);
  
  switch (l->current.kind) {
  case TOKEN_ID:
    size_t i = search_symbol(&fn->local, l->current.token);
    if (i < fn->local.count) {
      *arg = arg_local_var(fn, i);
    } else {
      arg->kind = ARG_NAME;
      arg->name = l->current.token;
      arg->loc = l->current.start;
      arg->type = (TypeExpr){.kind = TYPE_UNKNOWN};
    }
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

static bool compile_statement(Lexer *l, Program *prog, Fn *fn)
{
  Arg arg;
  Cursor loc = l->current.start;
  if (!compile_arg(l, prog, fn, &arg)) return false;
  if (!prefetch_not_none(l)) return false;

  if (l->current.kind == '(') {
    Op invoke = {
      .kind = OP_INVOKE,
      .loc = loc,
      .invoke = { .fn = arg },
    };
    
    if (!prefetch_not_none(l)) return false;
    while (l->current.kind != ')') {
      if (!compile_arg(l, prog, fn, &arg)) return false;
      if (!prefetch_not_none(l)) return false;
      da_append(&invoke.invoke.args, arg);
          
      if (l->current.kind != ',' && l->current.kind != ')') {
        pcompile_info(l->current.start, "error: expected ',' or ')', but got ");
        dump_token_kind(stderr, l->current.kind);
        free(invoke.invoke.args.items);
        return false;
      }
      
      if (l->current.kind == ',') {
        if (!prefetch_not_none(l)) return false;
      }
    }
    
    da_append(&fn->fn_body, invoke);
  } else if (l->current.kind == '=') {
    if (arg.kind != ARG_VAR_LOC) {
      pcompile_info(l->current.start, "error: try to assign to an rvalue\n");
      return false;
    };
    
    Arg val;
    if (!prefetch_not_none(l)) return false;
    if (!compile_arg(l, prog, fn, &val)) return false;
    
    Op set_var = {
      .kind = OP_SET_VAR,
      .loc = loc,
      .set_var = {
        .var = arg,
        .val = val,
      },
    };
    da_append(&fn->fn_body, set_var);
  } else {
    UNREACHABLE("");
  }
  
  return true;
}

static_assert(TYPE_KIND_COUNT == 6);
static struct {
  char *name;
  TypeExpr type;
} internal_types[] = {
  {
    .name = "void",
    .type = { .kind = TYPE_VOID, .size = 0, },
  },
  {
    .name = "u8",
    .type = { .kind = TYPE_UINT, .size = 1, },
  },
  {
    .name = "u16",
    .type = { .kind = TYPE_UINT, .size = 2, },
  },
  {
    .name = "u32",
    .type = { .kind = TYPE_UINT, .size = 4, },
  },
  {
    .name = "u64",
    .type = { .kind = TYPE_UINT, .size = 8, },
  },
  {
    .name = "i8",
    .type = { .kind = TYPE_INT, .size = 1, },
  },
  {
    .name = "i16",
    .type = { .kind = TYPE_INT, .size = 2, },
  },
  {
    .name = "i32",
    .type = { .kind = TYPE_INT, .size = 4, },
  },
  {
    .name = "i64",
    .type = { .kind = TYPE_INT, .size = 8, },
  }
};

static bool compile_internal_type(Lexer *l, TypeExpr *type) {
  assert(l->current.kind == TOKEN_ID);
  
  for (size_t i = 0; i < ARRAY_LEN(internal_types); ++i) {
    String_View name = sv_from_cstr(internal_types[i].name);
    if (sv_eq(name, l->current.token)) {
      *type = type_clone(internal_types[i].type);
      return true;
    }
  }
  pcompile_info(l->current.start, "error: unknown type "SV_Fmt"\n", SV_Arg(l->current.token));
  return false;
}

static bool compile_type_fn(Lexer *l, TypeExpr *type);

static bool compile_type_expr(Lexer *l, TypeExpr *type)
{
  switch (l->current.kind) {
  case TOKEN_FN:
    return compile_type_fn(l, type);
  case TOKEN_ID:
    return compile_internal_type(l, type);
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

static bool compile_local_var_singn(Lexer *l, Fn *fn, Symbol* var)
{
  bool result;

  if (!prefetch_expect_token(l, TOKEN_ID)) return_defer(false);

  if (symbol_defined(&fn->local, l->current.token)) {
    pcompile_info(l->current.start, "error: variable \""SV_Fmt"\" is alreay defined in this field\n",
                  SV_Arg(l->current.token));
    return_defer(false);
  }
  var->name = l->current.token;
  if (!prefetch_not_none(l)) return_defer(false);

  if (l->current.kind == ':') {
    if (!prefetch_not_none(l)) return_defer(false);
    if (!compile_type_expr(l, &var->type)) return_defer(false);
    if (var->type.kind == TYPE_VOID) {
      pcompile_info(l->current.start, "error: the type of a local variable cannot be \"void\"");
      return_defer(false);
    }
    if (!prefetch_not_none(l)) return_defer(false);
  }
  return_defer(true);
 defer:
  if (!result) destroy_symbol(var);
  return result;
}

static bool compile_local_var(Lexer *l, Program *prog, Fn *fn)
{
  Symbol var = {0};
  if (!compile_local_var_singn(l, fn, &var)) return false;
  da_append(&fn->local, var);
  size_t pos = fn->local.count - 1;
  
  if (l->current.kind != '=') return true;

  Cursor loc = l->current.start;
  if (!prefetch_not_none(l)) return false;
  Arg val = {0};
  if (!compile_arg(l, prog, fn, &val)) return false;    
  Op set_var = {
    .kind = OP_SET_VAR,
    .loc = loc,
    .set_var = {
      .var = arg_local_var(fn, pos),
      .val = val,
    },
  };
  da_append(&fn->fn_body, set_var);

  return prefetch_not_none(l);
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
      if (!compile_arg(l, prog, fn, &ret.ret_val)) return false;
      da_append(&fn->fn_body, ret);
      returned = true;
      if (!prefetch_not_none(l)) return false;
    } else if (l->current.kind == TOKEN_VAR) {
      if (!compile_local_var(l, prog, fn)) return false;
    } else {
      if (!compile_statement(l, prog, fn)) return false;
      if (!prefetch_not_none(l)) return false;
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

static bool compile_function(Lexer *l, Program *prog)
{
  assert(l->current.kind == TOKEN_FN);

  if (!prefetch_expect_token(l, TOKEN_ID)) return false;

  if (symbol_defined(&prog->global, l->current.token)) {
    pcompile_info(l->current.start, "error: symbol "SV_Fmt" redefined\n", SV_Arg(l->current.token));
    return false;
  }

  Fn fn = {
    .name = l->current.token,
  };
  
  if (!prefetch_expect_token(l, '(')) return false;
  if (!prefetch_expect_token(l, ')')) return false;
  
  if (!prefetch_expect_token(l, ':')) return false;
  if (!prefetch_not_none(l)) return false;

  TypeExpr ret_type;
  if (!compile_type_expr(l, &ret_type)) return false;
  
  if (!prefetch_expect_token(l, '{')) return false;

  if (!compile_fn_body(l, prog, &fn)) return false;
  
  if (!expect_token(l, '}')) return false;

  da_append(&prog->fn_list, fn);

  Symbol sym = {
    .name = fn.name,
    .type = (TypeExpr) {
      .kind = TYPE_FN,
      .fn_type = {
        .ret_type = malloc(sizeof(TypeExpr)),
        .arg_types = {0},
        .va_args = false,
      },
    },
  };
  *sym.type.fn_type.ret_type = ret_type;
  da_append(&prog->global, sym);

  return true;
}

static bool name_arg_defined(Program *prog, Arg *arg)
{
  if (arg->kind == ARG_NAME && !symbol_defined(&prog->global, arg->name)) {
    pcompile_info(arg->loc,
                  "error: refered symbol \"%s\" is not defined\n", arg->name);
    return false;
  }
  return true;
}

static bool all_refered_defined(Program *prog)
{
  da_foreach (Fn, fn, &prog->fn_list) {
    da_foreach (Op, op, &fn->fn_body) {
      switch (op->kind) {
      case OP_RETURN:
        if (!name_arg_defined(prog, &op->ret_val)) return false;
        break;
      case OP_SET_VAR:
        if (!name_arg_defined(prog, &op->set_var.val)) return false;
        if (!name_arg_defined(prog, &op->set_var.var)) return false;
        break;
      case OP_INVOKE:
        if (!name_arg_defined(prog, &op->invoke.fn)) return false;
        da_foreach (Arg, arg, &op->invoke.args) {
          if (!name_arg_defined(prog, arg)) return false;
        }
        break;
      default: UNREACHABLE("op");
      }
    }
  }
  return true;
}

static bool detect_arg_type(Arg *arg, SymbolList *global, SymbolList *local) {
  if (arg->type.kind != TYPE_UNKNOWN) return true;

  if (arg->kind == ARG_NAME) {
    assert(symbol_defined(global, arg->name));
    size_t loc = search_symbol(global, arg->name);
    arg->type = type_clone(global->items[loc].type);
  } else if (arg->kind == ARG_VAR_LOC) {
    assert(arg->label - 1 < local->count);
    TypeExpr *var_type = &local->items[arg->label - 1].type;
    assert(var_type->kind != TYPE_UNKNOWN);
    arg->type = type_clone(*var_type);
  } else {
    UNREACHABLE("detect_arg_type");
  }
  return true;
}

static bool detect_var_type(Arg *var, Arg *val, SymbolList *global, SymbolList *local)
{
  assert(val->type.kind != TYPE_UNKNOWN);
  if (var->type.kind != TYPE_UNKNOWN) return true;
  
  TypeExpr *var_type = NULL;
  if (var->kind == ARG_VAR_LOC) {
    assert(var->label - 1 < local->count);
    var_type = &local->items[var->label - 1].type;
  } else if (var->kind == ARG_NAME) {
    assert(symbol_defined(global, var->name));
    size_t i = search_symbol(global, var->name);
    var_type = &global->items[i].type;
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
  SymbolList *global = &prog->global;
  da_foreach (Fn, fn, &prog->fn_list) {
    SymbolList *local = &fn->local;
    da_foreach (Op, op, &fn->fn_body) {
      switch (op->kind) {
      case OP_RETURN:
        if (!detect_arg_type(&op->ret_val, global, local)) return false;
        break;
      case OP_SET_VAR:
        if (!detect_arg_type(&op->set_var.val, global, local)) return false;
        if (!detect_var_type(&op->set_var.var, &op->set_var.val, global, local)) return false;
        break;
      case OP_INVOKE:
        if (!detect_arg_type(&op->invoke.fn, global, local)) return false;
        da_foreach (Arg, arg, &op->invoke.args) {
          if (!detect_arg_type(arg, global, local)) return false;
        }
        break;
      default: UNREACHABLE("op");
      }
    }
  }
  return true;
}

static bool compile_file(Lexer *l, Program *prog)
{
  while (next_token(l)) {
    if (l->current.kind == ';') {
      continue;
    }
    
    if (l->current.kind == TOKEN_FN) {
      if (!compile_function(l, prog)) return false;
    } else if (l->current.kind == TOKEN_EXT) {
      Symbol sym = { .external = true };
      if (!prefetch_expect_token(l, TOKEN_ID)) return false;
      if (symbol_defined(&prog->global, l->current.token)) {
        pcompile_info(l->current.start, "error: symbol "SV_Fmt" redefined\n", SV_Arg(l->current.token));
        return false;
      }
      sym.name = l->current.token;
      if (!prefetch_expect_token(l, ':')) return false;
      if (!prefetch_not_none(l)) return false;
      if (!compile_type_expr(l, &sym.type)) return false;

      da_append(&prog->global, sym);
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

static void dump_type_expr(TypeExpr *type, FILE *stream)
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
    assert(symbol_defined(&prog->global, fn->name));
    size_t fn_loc = search_symbol(&prog->global, fn->name);
    TypeExpr *fn_type = &prog->global.items[fn_loc].type;
    
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

bool compile_program(Lexer *l, Program *prog)
{
  if (!compile_file(l, prog)) return false;
  if (!all_refered_defined(prog)) return false;
  if (!detect_all_unknown_type(prog)) return false;
  if (!check_type(prog)) return false;
  
  return true;
}

void destroy_program(Program *prog)
{
  da_foreach (Symbol, sym, &prog->global) {
    destroy_symbol(sym);
  }
  da_free(prog->global);

  destroy_fn_list(&prog->fn_list);
  if (prog->str_lits.capacity > 0) {
    da_free(prog->str_lits);
  }
}

