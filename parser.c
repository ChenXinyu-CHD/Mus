#include "parser.h"

#include "nob.h"
#define STB_C_LEXER_IMPLEMENTATION
#include "stb_c_lexer.h"

static char *token_name(long token)
{
  switch (token) {
  case CLEX_id          : return temp_strdup("id");
  case CLEX_eq          : return temp_strdup("\"==\"");
  case CLEX_noteq       : return temp_strdup("\"!=\"");
  case CLEX_lesseq      : return temp_strdup("\"<=\"");
  case CLEX_greatereq   : return temp_strdup("\">=\"");
  case CLEX_andand      : return temp_strdup("\"&&\"");
  case CLEX_oror        : return temp_strdup("\"||\"");
  case CLEX_shl         : return temp_strdup("\"<<\"");
  case CLEX_shr         : return temp_strdup("\">>\"");
  case CLEX_plusplus    : return temp_strdup("\"++\"");
  case CLEX_minusminus  : return temp_strdup("\"--\"");
  case CLEX_arrow       : return temp_strdup("\"->\"");
  case CLEX_andeq       : return temp_strdup("\"&=\"");
  case CLEX_oreq        : return temp_strdup("\"|=\"");
  case CLEX_xoreq       : return temp_strdup("\"^=\"");
  case CLEX_pluseq      : return temp_strdup("\"+=\"");
  case CLEX_minuseq     : return temp_strdup("\"-=\"");
  case CLEX_muleq       : return temp_strdup("\"*=\"");
  case CLEX_diveq       : return temp_strdup("\"/=\"");
  case CLEX_modeq       : return temp_strdup("\"%=\"");
  case CLEX_shleq       : return temp_strdup("\"<<=\"");
  case CLEX_shreq       : return temp_strdup("\">>=\"");
  case CLEX_eqarrow     : return temp_strdup("\"=>\"");
  case CLEX_dqstring    : return temp_strdup("dqstring");
  case CLEX_sqstring    : return temp_strdup("sqstring");
  case CLEX_charlit     : return temp_strdup("character literal");
  case CLEX_intlit      : return temp_strdup("integer literal");
  case CLEX_floatlit    : return temp_strdup("float literal");
  case CLEX_eof         : return temp_strdup("EOF");
  case CLEX_parse_error : return temp_strdup("ERROR");
  default:
    if (token >= 0 && token < 256) {
      char* str = temp_strdup("\' \'");
      str[1] = (char)token;
      return str;
    } else {
      UNREACHABLE("token");
    }
  }
}

static stb_lex_location lex_location(stb_lexer *l)
{
  stb_lex_location loc = {0};
  stb_c_lexer_get_location(l, l->where_lastchar, &loc);
  return loc;
}

static void vpcompile_info(const char *filename, stb_lex_location loc, const char* fmt, va_list args)
{
  size_t mark = temp_save(); {
    va_list ap;
    va_copy(ap, args);
    char *msg = temp_vsprintf(fmt, ap);
    va_end(ap);
  
    fprintf(stderr, "%s:%d:%d: %s",
            filename, loc.line_number, loc.line_offset, msg);
  } temp_rewind(mark);
}

static void pcompile_info(const char *filename, stb_lex_location loc, const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vpcompile_info(filename, loc, fmt, args);
  va_end(args);
}

static bool next_token(stb_lexer *l, const char *filename)
{
  bool result = stb_c_lexer_get_token(l);
  if (l->token == CLEX_parse_error) {
    pcompile_info(filename, lex_location(l), "error: parse error when read \"%.*s\"\n",
                  (int)(l->where_lastchar - l->where_firstchar), l->where_firstchar);
  }

  return result;
}

static bool prefetch_not_none(stb_lexer *l, const char *filename)
{
  bool result = next_token(l, filename);
  if (l->token == CLEX_eof) {
    pcompile_info(filename, lex_location(l), "error: unexpected EOF\n");
    return false;
  }
  
  return result;
}

static bool expect_token(stb_lexer *l, const char *filename, long token)
{
  if (l->token == token) return true;
  
  size_t mark = temp_save(); {
    pcompile_info(filename, lex_location(l), "error: expect token %s but got %s\n",
                  token_name(token), token_name(l->token));
  } temp_rewind(mark);
  return false;
}

static bool prefetch_expect_token(stb_lexer *l, const char *filename, long token)
{
  next_token(l, filename);
  if (!expect_token(l, filename, token)) return false;
  return true;
}

static bool prefetch_expect_tokens_(stb_lexer *l, const char *filename, ...)
{
  next_token(l, filename);
  bool found = false;
  
  va_list ap; va_start(ap, filename); {
    long arg = va_arg(ap, long);
    while (arg != CLEX_parse_error) {
      if (arg == l->token) {
        found = true;
        break;
      }
      arg = va_arg(ap, long);
    }
  } va_end(ap);

  if (found) return true;

  va_start(ap, filename); size_t mark = temp_save(); {
    pcompile_info(filename, lex_location(l), "error: expect token ");
    
    long arg = va_arg(ap, long);
    while (arg != CLEX_parse_error) {
      fprintf(stderr, "%s, ", token_name(arg));
      arg = va_arg(ap, long);
    }
    fprintf(stderr, "but got %s\n", token_name(l->token));
  } temp_rewind(mark); va_end(ap);
  
  return false;
}

#define prefetch_expect_tokens(l, filename, ...)                        \
  prefetch_expect_tokens_(l, filename, __VA_ARGS__, CLEX_parse_error)

static bool expect_ids_(stb_lexer *l, const char *filename, ...)
{
  if (!expect_token(l, filename, CLEX_id)) return false;
  
  bool found = false;
  
  va_list ap; va_start(ap, filename); {
    char *arg = va_arg(ap, char*);
    while (arg != NULL) {
      if (strcmp(arg, l->string) == 0) {
        found = true;
        break;
      }
      arg = va_arg(ap, char*);
    }
  } va_end(ap);

  if (found) return true;

  va_start(ap, filename); size_t mark = temp_save(); {
    pcompile_info(filename, lex_location(l), "error: expect ");

    char* arg = va_arg(ap, char*);
    while (arg != NULL) {
      fprintf(stderr, "%s, ", arg);
      arg = va_arg(ap, char*);
    }
    fprintf(stderr, "but got %s\n", l->string);
  } temp_rewind(mark); va_end(ap);
  
  return false;
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
    assert(type->ret_type != NULL);
    destroy_type_expr(type->ret_type);
    free(type->ret_type);
    
    da_foreach (TypeExpr, arg, &type->arg_types) {
      destroy_type_expr(arg);
    }
    if (type->arg_types.count > 0) da_free(type->arg_types);
  } break;
  default: UNREACHABLE("type");
  }
}

static void destroy_symbol(Symbol *sym)
{
  if (sym->name != NULL) free(sym->name);
  destroy_type_expr(&sym->type);
}

static void destroy_arg(Arg *arg)
{
  destroy_type_expr(&arg->type);
  switch(arg->kind) {
  case ARG_NONE: break;
  case ARG_NAME: free(arg->name); break;
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
    destroy_arg(&op->fn);
    da_foreach(Arg, arg, &op->args) {
      destroy_arg(arg);
    }
    break;
  case OP_RETURN: destroy_arg(&op->ret_val); break;
  case OP_SET_VAR:
    destroy_arg(&op->var);
    destroy_arg(&op->val);
    break;
  default: UNREACHABLE("destroy op");
  }
}

static void destroy_fn_list(FnList *fn_list)
{
  da_foreach (Fn, fn, fn_list) {
    free(fn->name);
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

#define BASIC_TYPE_NAMES                        \
  "i8",                                         \
    "i16",                                      \
    "i32",                                      \
    "i64",                                      \
    "i32",                                      \
    "u8",                                       \
    "u16",                                      \
    "u32",                                      \
    "u64",                                      \
    "u32",                                      \
    "void"

static char *keywords[] = {
  "extern",
  "fn",
  "return",
  "var",
  BASIC_TYPE_NAMES
};

static bool is_keywords(const char *str)
{
  for (size_t i = 0; i < ARRAY_LEN(keywords); ++i) {
    if (strcmp(str, keywords[i]) == 0) return true;
  }

  return false;
}

#define expect_ids(l, filename, ...)            \
  expect_ids_(l, filename, __VA_ARGS__, NULL)

static size_t search_symbol(SymbolList *syms, const char *name)
{
  for (size_t i = 0; i < syms->count; ++i) {
    if (strcmp(syms->items[i].name, name) == 0) {
      return i;
    }
  }
  
  return syms->count;
}

static bool symbol_defined(SymbolList *syms, const char *name)
{
  return search_symbol(syms, name) < syms->count;
}

static size_t compile_strlit(Program *prog, char *str)
{
  size_t str_count = prog->str_lits.count;
  for (size_t i = 0; i < str_count; ++i) {
    if (strcmp(prog->str_lits.items[i], str) == 0) {
      return i;
    }
  }
  
  da_append(&prog->str_lits, strdup(str));
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
    .ret_type = malloc(sizeof(TypeExpr)),
    .arg_types = arg_types,
    .va_args = va_args,
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
    da_foreach (TypeExpr, arg, &t.arg_types) {
      da_append(&arg_types, type_clone(*arg));
    }
    result = fn_type(*t.ret_type, arg_types, t.va_args);
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

static bool compile_arg(stb_lexer *l, const char *filename, Program *prog, Fn *fn, Arg *arg)
{
  UNUSED(prog);
  assert(l->token != CLEX_eof);
  
  switch (l->token) {
  case CLEX_id:
    size_t i = search_symbol(&fn->local, l->string);
    if (i < fn->local.count) {
      *arg = arg_local_var(fn, i);
    } else {
      arg->kind = ARG_NAME;
      arg->name = strdup(l->string);
      stb_c_lexer_get_location(l, l->where_lastchar, &arg->loc);
      arg->type = (TypeExpr){.kind = TYPE_UNKNOWN};
    }
    break;
  case CLEX_dqstring:
    arg->kind = ARG_LIT_STR;
    arg->label = compile_strlit(prog, l->string);
    arg->type = ptr_type((TypeExpr) {
        .kind = TYPE_INT,
        .size = 1,
      });
    break;
  case CLEX_charlit:
    TODO("CLEX_charlit");
    break;
  case CLEX_intlit:
    arg->kind = ARG_LIT_INT;
    arg->num_int = l->int_number;
    arg->type = (TypeExpr) {
      .kind = TYPE_INT,
      .size = 4,
    };
    break;
  case CLEX_floatlit:
    TODO("CLEX_floatlit");
    break;
  default:
    size_t mark = temp_save(); {
      pcompile_info(filename, lex_location(l), "error: invalid token %s in an argument\n",
                    token_name(l->token));
    } temp_rewind(mark);
    return false;
  }

  return true;
}

static bool compile_statement(stb_lexer *l, const char *filename, Program *prog, Fn *fn)
{
  Arg arg;
  stb_lex_location loc = lex_location(l);
  if (!compile_arg(l, filename, prog, fn, &arg)) return false;
  if (!prefetch_not_none(l, filename)) return false;

  if (l->token == '(') {
    Op invoke = {
      .kind = OP_INVOKE,
      .loc = loc,
      .fn = arg
    };
    
    if (!prefetch_not_none(l, filename)) return false;
    while (l->token != ')') {
      if (!compile_arg(l, filename, prog, fn, &arg)) return false;
      if (!prefetch_not_none(l, filename)) return false;
      da_append(&invoke.args, arg);
          
      if (l->token != ',' && l->token != ')') {
        size_t mark = temp_save(); {
          pcompile_info(filename, lex_location(l), "error: expected ',' or ')', but got %s\n",
                        token_name(l->token));
        } temp_rewind(mark);
        free(invoke.args.items);
        return false;
      }
      
      if (l->token == ',') {
        if (!prefetch_not_none(l, filename)) return false;
      }
    }
    
    da_append(&fn->fn_body, invoke);
  } else if (l->token == '=') {
    if (arg.kind != ARG_VAR_LOC) {
      pcompile_info(filename, lex_location(l), "error: try to assign to an rvalue\n");
      return false;
    };
    
    Arg val;
    if (!prefetch_not_none(l, filename)) return false;
    if (!compile_arg(l, filename, prog, fn, &val)) return false;
    
    Op set_var = {
      .kind = OP_SET_VAR,
      .loc = loc,
      .var = arg,
      .val = val,
    };
    da_append(&fn->fn_body, set_var);
  } else {
    UNREACHABLE(token_name(l->token));
  }
  
  return true;
}

static bool compile_internal_type(stb_lexer *l, const char *filename, TypeExpr *type) {
  // TODO: support more internal type
  if (!expect_ids(l, filename, BASIC_TYPE_NAMES)) { return false; }

  if (strcmp(l->string, "void") == 0) {
    type->kind = TYPE_VOID;
    return true;
  }
  
  switch(l->string[0]) {
  case 'i': type->kind = TYPE_INT; break;
  case 'u': type->kind = TYPE_UINT; break;
  default: UNREACHABLE("l->string");
  }
  
  size_t bitwide = 0;
  char *it = l->string + 1;
  while (*it != '\0') {
    if (!isdigit(*it)) return false;
    bitwide = bitwide * 10 + *it++ - '0';
  }

  switch(bitwide) {
  case 8:  type->size = 1; break;
  case 16: type->size = 2; break;
  case 32: type->size = 4; break;
  case 64: type->size = 8; break;
  default: UNREACHABLE("Unsipported size");
  }

  return true;
}

static bool compile_type_fn(stb_lexer *l, const char *filename, TypeExpr *type);

static bool compile_type_expr(stb_lexer *l, const char *filename, TypeExpr *type)
{
  switch (l->token) {
  case CLEX_id: {
    if (strcmp(l->string, "fn") == 0) return compile_type_fn(l, filename, type);
    else return compile_internal_type(l, filename, type);
  }
  case '&': {
    if (!prefetch_not_none(l, filename)) return false;
    TypeExpr ref_type;
    if (!compile_type_expr(l, filename, &ref_type)) return false;
    *type = ptr_type(ref_type);
    return true;
  }
  default:
    size_t mark = temp_save(); {
      pcompile_info(filename, lex_location(l), "error: expected a type but got %s\n", token_name(l->token));
    } temp_rewind(mark);
    return false;
  }
}

static bool compile_type_fn(stb_lexer *l, const char *filename, TypeExpr *type)
{
  assert(l->token == CLEX_id && strcmp(l->string, "fn") == 0);

  bool result;

  type->kind = TYPE_FN;
  if (!prefetch_expect_token(l, filename, '(')) return_defer(false);

  if (!prefetch_not_none(l, filename)) return_defer(false);
  type->arg_types = (TypeList) {0};
  while (l->token != ')') {
    if (l->token == '.') { // parse "..." for va_args
      if (!prefetch_expect_token(l, filename, '.')) return false;
      if (!prefetch_expect_token(l, filename, '.')) return false;
      // va_args must be the last argument
      if (!prefetch_expect_token(l, filename, ')')) return false;
      type->va_args = true;
    } else {
      // failures in this loop would not cause memory leak;
      TypeExpr arg_type = {0};
      // memory would clean up in the compile_type_expr;
      if (!compile_type_expr(l, filename, &arg_type)) return_defer(false);
      da_append(&type->arg_types, arg_type);
      // from here, the ownership has moved to ext.type.arg_types;
      // thus, they will be clean up together with ext.type.arg_types;
      if (!prefetch_expect_tokens(l, filename, ',', ')')) return_defer(false);
      if (l->token == ',') {
        if (!prefetch_not_none(l, filename)) return_defer(false);
      }
    }
  }

  if (!prefetch_expect_token(l, filename, ':')) return_defer(false);
  if (!prefetch_not_none(l, filename)) return_defer(false);
  type->ret_type = calloc(1, sizeof(TypeExpr));
  if (!compile_type_expr(l, filename, type->ret_type)) return_defer(false);

  return_defer(true);

 defer:
  if (!result) destroy_type_expr(type);
  return result;
}

static bool compile_local_var_singn(stb_lexer *l, const char *filename, Fn *fn, Symbol* var)
{
  bool result;

  if (!prefetch_expect_token(l, filename, CLEX_id)) return_defer(false);
  if (is_keywords(l->string)) {
    pcompile_info(filename, lex_location(l), "error: expected an id, but got a keyword \"%s\"\n", l->string);
    return_defer(false);
  }

  if (symbol_defined(&fn->local, l->string)) {
    pcompile_info(filename, lex_location(l), "error: variable \"%s\" is alreay defined in this field\n", l->string);
    return_defer(false);
  }
  var->name = strdup(l->string);
  if (!prefetch_not_none(l, filename)) return_defer(false);

  if (l->token == ':') {
    if (!prefetch_not_none(l, filename)) return_defer(false);
    if (!compile_type_expr(l, filename, &var->type)) return_defer(false);
    if (var->type.kind == TYPE_VOID) {
      pcompile_info(filename, lex_location(l), "error: the type of a local variable cannot be \"void\"");
      return_defer(false);
    }
    if (!prefetch_not_none(l, filename)) return_defer(false);
  }
  return_defer(true);
 defer:
  if (!result) destroy_symbol(var);
  return result;
}

static bool compile_local_var(stb_lexer *l, const char *filename, Program *prog, Fn *fn)
{
  Symbol var = {0};
  if (!compile_local_var_singn(l, filename, fn, &var)) return false;
  da_append(&fn->local, var);
  size_t pos = fn->local.count - 1;
  
  if (l->token != '=') return true;

  stb_lex_location loc = lex_location(l);
  if (!prefetch_not_none(l, filename)) return false;
  Arg val = {0};
  if (!compile_arg(l, filename, prog, fn, &val)) return false;    
  Op set_var = {
    .kind = OP_SET_VAR,
    .loc = loc,
    .var = arg_local_var(fn, pos),
    .val = val,
  };
  da_append(&fn->fn_body, set_var);

  return prefetch_not_none(l, filename);
}

static bool compile_fn_body(stb_lexer *l, const char *filename, Program *prog, Fn *fn) {
  assert(l->token == '{');

  bool returned = false;
  if (!prefetch_not_none(l, filename)) return false;
  while (l->token != '}') {
    if (l->token == ';') {
      if (!prefetch_not_none(l, filename)) return false;
    } else if (l->token == CLEX_id && strcmp(l->string, "return") == 0) {
      Op ret = {
        .kind = OP_RETURN,
        .loc = lex_location(l),
      };
      if (!prefetch_not_none(l, filename)) return false;
      if (!compile_arg(l, filename, prog, fn, &ret.ret_val)) return false;
      da_append(&fn->fn_body, ret);
      returned = true;
      if (!prefetch_not_none(l, filename)) return false;
    } else if (l->token == CLEX_id && strcmp(l->string, "var") == 0) {
      if (!compile_local_var(l, filename, prog, fn)) return false;
    } else {
      if (!compile_statement(l, filename, prog, fn)) return false;
      if (!prefetch_not_none(l, filename)) return false;
    }
  }

  if (!returned) {
    Op ret = {
      .kind = OP_RETURN,
      .loc = lex_location(l),
      .ret_val = {
        .kind = ARG_NONE,
        .type = { .kind = TYPE_VOID },
      },
    };
    da_append(&fn->fn_body, ret);
  }

  return true;
}

static bool compile_function(stb_lexer *l, const char *filename, Program *prog)
{
  assert(expect_token(l, filename, CLEX_id) && strcmp(l->string, "fn") == 0);

  if (!prefetch_expect_token(l, filename, CLEX_id)) return false;

  if (symbol_defined(&prog->global, l->string)) {
    pcompile_info(filename, lex_location(l), "error: symbol %s redefined\n", l->string);
    return false;
  }

  if (is_keywords(l->string)) {
    pcompile_info(filename, lex_location(l), "error: expected an id, but got a keyword \"%s\"\n", l->string);
    return false;
  }

  Fn fn = {
    .name = strdup(l->string),
  };
  
  if (!prefetch_expect_token(l, filename, '(')) return false;
  if (!prefetch_expect_token(l, filename, ')')) return false;
  
  if (!prefetch_expect_token(l, filename, ':')) return false;
  if (!prefetch_not_none(l, filename)) return false;

  TypeExpr ret_type;
  if (!compile_type_expr(l, filename, &ret_type)) return false;
  
  if (!prefetch_expect_token(l, filename, '{')) return false;

  if (!compile_fn_body(l, filename, prog, &fn)) return false;
  
  if (!expect_token(l, filename, '}')) return false;

  da_append(&prog->fn_list, fn);

  Symbol sym = {
    .name = strdup(fn.name),
    .type = (TypeExpr) {
      .kind = TYPE_FN,
      .ret_type = malloc(sizeof(TypeExpr)),
      .arg_types = {0},
      .va_args = false,
    },
  };
  *sym.type.ret_type = ret_type;
  da_append(&prog->global, sym);

  return true;
}

static bool name_arg_defined(Program *prog, Arg *arg)
{
  if (arg->kind == ARG_NAME && !symbol_defined(&prog->global, arg->name)) {
    pcompile_info(prog->filename, arg->loc,
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
        if (!name_arg_defined(prog, &op->val)) return false;
        if (!name_arg_defined(prog, &op->var)) return false;
        break;
      case OP_INVOKE:
        if (!name_arg_defined(prog, &op->fn)) return false;
        da_foreach (Arg, arg, &op->args) {
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
        if (!detect_arg_type(&op->val, global, local)) return false;
        if (!detect_var_type(&op->var, &op->val, global, local)) return false;
        break;
      case OP_INVOKE:
        if (!detect_arg_type(&op->fn, global, local)) return false;
        da_foreach (Arg, arg, &op->args) {
          if (!detect_arg_type(arg, global, local)) return false;
        }
        break;
      default: UNREACHABLE("op");
      }
    }
  }
  return true;
}

static bool compile_file(stb_lexer *l, const char *filename, Program *prog)
{
  prog->filename = filename;
  while (next_token(l, filename)) {
    if (l->token == ';') {
      continue;
    }
    
    if (!expect_ids(l, filename, "fn", "extern")) return false;
    
    if (strcmp(l->string, "fn") == 0) {
      if (!compile_function(l, filename, prog)) return false;
    } else if (strcmp(l->string, "extern") == 0) {
      Symbol sym = { .external = true };
      if (!prefetch_expect_token(l, filename, CLEX_id)) return false;
      if (symbol_defined(&prog->global, l->string)) {
        pcompile_info(filename, lex_location(l), "error: symbol %s redefined\n", l->string);
        return false;
      }
      if (is_keywords(l->string)) {
        pcompile_info(filename, lex_location(l), "error: expected an id, but got a keyword \"%s\"\n", l->string);
        return false;
      }
      sym.name = strdup(l->string);
      if (!prefetch_expect_token(l, filename, ':')) return false;
      if (!prefetch_not_none(l, filename)) return false;
      if (!compile_type_expr(l, filename, &sym.type)) return false;

      da_append(&prog->global, sym);
    } else {
      UNREACHABLE(temp_sprintf("unexpected id: %s", l->string));
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
    for (size_t i = 0; i < type->arg_types.count; ++i) {
      dump_type_expr(&type->arg_types.items[i], stream);
      if (i + 1 < type->arg_types.count) {
        fprintf(stream, ",");
      } else if (type->va_args) {
        fprintf(stream, ",...");
      }
    }
    fprintf(stream, "):");
    dump_type_expr(type->ret_type, stream);
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
        if (!type_matched(fn_type->ret_type, ret_type)) {
          pcompile_info(prog->filename, op->loc, "error: the return type of %s is required to be ", fn->name);
          dump_type_expr(fn_type->ret_type, stderr);
          fprintf(stderr, ", but got ");
          dump_type_expr(ret_type, stderr);
          fputc('\n', stderr);
          return false;
        }
        break;
      case OP_SET_VAR:
        if (!type_matched(&op->var.type, &op->val.type)) {
          pcompile_info(prog->filename, op->loc, "error: incompatible types when assigning to type \"");
          dump_type_expr(&op->var.type, stderr);
          fprintf(stderr, "\" from type \"");
          dump_type_expr(&op->val.type, stderr);
          fprintf(stderr, "\"\n");
          return false;
        }
        
        break;
      case OP_INVOKE:
        // TODO: report a better error message.
        if (op->fn.type.kind != TYPE_FN) {
          pcompile_info(prog->filename, op->loc, "error: try to invoke an uncallable value\n");
          return false;
        }
        TypeExpr *invoked_type = &op->fn.type;
        TypeList *expected_types = &invoked_type->arg_types;

        bool size_matched = invoked_type->va_args?
          expected_types->count <= op->args.count:
          expected_types->count == op->args.count;
        if (!size_matched) {
          pcompile_info(prog->filename, op->loc,
                        "error: this function expected %ld arguments, but got %ld arguments\n",
                        expected_types->count, op->args.count);
          return false;
        }

        assert(expected_types->count <= op->args.count);
        for (size_t i = 0; i < expected_types->count; ++i) {
          TypeExpr *expected = &expected_types->items[i];
          TypeExpr *actual = &op->args.items[i].type;
          if (!type_matched(expected, actual)) {
            pcompile_info(prog->filename, op->loc, "error: the %ld-th argument is expected to be ", i);
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

bool compile_program(stb_lexer *l, const char *filename, Program *prog)
{
  if (!compile_file(l, filename, prog)) return false;
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
    da_foreach (char *, str, &prog->str_lits) {
      free(*str);
    }
    da_free(prog->str_lits);
  }
}

