#include "parser.h"

#include "nob.h"
#define STB_C_LEXER_IMPLEMENTATION
#include "stb_c_lexer.h"

char *token_name(long token)
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

void vpcompile_info(stb_lexer* l, const char *filename, const char* fmt, va_list args)
{
  size_t mark = temp_save(); {
    va_list ap;
    va_copy(ap, args);
    char *msg = temp_vsprintf(fmt, ap);
    va_end(ap);
  
    stb_lex_location loc = {0};
    stb_c_lexer_get_location(l, l->where_lastchar, &loc);
    fprintf(stderr, "%s:%d:%d: %s",
            filename, loc.line_number, loc.line_offset, msg);
  } temp_rewind(mark);
}

void pcompile_info(stb_lexer* l, const char *filename, const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vpcompile_info(l, filename, fmt, args);
  va_end(args);
}

bool next_token(stb_lexer *l, const char *filename)
{
  bool result = stb_c_lexer_get_token(l);
  if (l->token == CLEX_parse_error) {
    pcompile_info(l, filename, "error: parse error when read \"%.*s\"\n",
                  (int)(l->where_lastchar - l->where_lastchar), l->where_lastchar);
  }

  return result;
}

bool prefetch_not_none(stb_lexer *l, const char *filename)
{
  bool result = next_token(l, filename);
  if (l->token == CLEX_eof) {
    printf("EOF\n");
    pcompile_info(l, filename, "error: unexpected EOF\n",
                  (int)(l->where_lastchar - l->where_lastchar), l->where_lastchar);
    return false;
  }
  
  return result;
}

bool expect_token(stb_lexer *l, const char *filename, long token)
{
  if (l->token == token) return true;
  
  size_t mark = temp_save(); {
    pcompile_info(l, filename, "error: expect token %s but got %s\n",
                  token_name(token), token_name(l->token));
  } temp_rewind(mark);
  return false;
}

bool prefetch_expect_token(stb_lexer *l, const char *filename, long token)
{
  next_token(l, filename);
  if (!expect_token(l, filename, token)) return false;
  return true;
}

bool prefetch_expect_tokens_(stb_lexer *l, const char *filename, ...)
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
    pcompile_info(l, filename, "error: expect token ");
    
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

bool expect_ids_(stb_lexer *l, const char *filename, ...)
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
    pcompile_info(l, filename, "error: expect ");

    char* arg = va_arg(ap, char*);
    while (arg != NULL) {
      fprintf(stderr, "%s, ", arg);
      arg = va_arg(ap, char*);
    }
    fprintf(stderr, "but got %s\n", l->string);
  } temp_rewind(mark); va_end(ap);
  
  return false;
}

#define expect_ids(l, filename, ...)            \
  expect_ids_(l, filename, __VA_ARGS__, NULL)


bool symbol_defined(Program *prog, char *name)
{
  for (size_t i = 0; i < prog->externs.count; ++i) {
    if (strcmp(prog->externs.items[i].name, name) == 0) {
      return true;
    }
  }
  
  for (size_t i = 0; i < prog->fn_list.count; ++i) {
    if (strcmp(prog->fn_list.items[i].name, name) == 0) {
      return true;
    }
  }

  return false;
}

size_t compile_strlit(Program *prog, char *str)
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

bool get_local_var(const VarList *var_list, Arg* arg, const char *name)
{
  size_t label = 0;
  while (label < var_list->count) {
    if (strcmp(name, var_list->items[label].name) == 0) {
      arg->kind = ARG_VAR_LOC;
      arg->label = label + 1;
      return true;
    }
    label += 1;
  }
  return false;
}

bool compile_arg(stb_lexer *l, const char *filename, Program *prog, Fn *fn, Arg *arg)
{
  UNUSED(prog);
  assert(l->token != CLEX_eof);
  
  switch (l->token) {
  case CLEX_id:
    if (!get_local_var(&fn->local, arg, l->string)) {
      arg->kind = ARG_NAME;
      arg->name = strdup(l->string);
    }
    break;
  case CLEX_dqstring:
    arg->kind = ARG_LIT_STR;
    arg->label = compile_strlit(prog, l->string);
    break;
  case CLEX_charlit:
    TODO("CLEX_charlit");
    break;
  case CLEX_intlit:
    arg->kind = ARG_LIT_INT;
    arg->num_int = l->int_number;
    break;
  case CLEX_floatlit:
    TODO("CLEX_floatlit");
    break;
  default:
    size_t mark = temp_save(); {
      pcompile_info(l, filename, "error: invalid token %s in an argument\n",
                    token_name(l->token));
    } temp_rewind(mark);
    return false;
  }

  return true;
}

bool compile_statement(stb_lexer *l, const char *filename, Program *prog, Fn *fn)
{
  Arg arg;
  if (!compile_arg(l, filename, prog, fn, &arg)) return false;
  if (!prefetch_not_none(l, filename)) return false;

  if (l->token == '(') {
    Op invoke = { .kind = OP_INVOKE, .fn = arg };
    
    if (!prefetch_not_none(l, filename)) return false;
    while (l->token != ')') {
      if (!compile_arg(l, filename, prog, fn, &arg)) return false;
      if (!prefetch_not_none(l, filename)) return false;
      da_append(&invoke.args, arg);
          
      if (l->token != ',' && l->token != ')') {
        size_t mark = temp_save(); {
          pcompile_info(l, filename, "error: expected ',' or ')', but got %s\n",
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
      pcompile_info(l, filename, "error: try to assign to an rvalue\n");
      return false;
    };
    
    Arg val;
    if (!prefetch_not_none(l, filename)) return false;
    if (!compile_arg(l, filename, prog, fn, &val)) return false;
    
    Op set_var = {
      .kind = OP_SET_VAR,
      .var = arg,
      .val = val,
    };
    da_append(&fn->fn_body, set_var);
  } else {
    UNREACHABLE(token_name(l->token));
  }
  
  return true;
}

void destroy_type_expr(TypeExpr* type)
{
  if (type == NULL) return;
  
  switch(type->kind) {
  case TYPE_INT: break;
  case TYPE_UINT: break;
  case TYPE_VOID: break;
  case TYPE_PTR:
    destroy_type_expr(type->ref_type);
    if (type->ref_type != NULL) free(type->ref_type);
    break;
  case TYPE_FN: {
    destroy_type_expr(type->ret_type);
    if (type->ret_type != NULL) free(type->ret_type);
    
    da_foreach (TypeExpr, arg, &type->arg_types) {
      destroy_type_expr(arg);
    }
    if (type->arg_types.count > 0) da_free(type->arg_types);
  } break;
  default: UNREACHABLE("type");
  }
}

bool compile_internal_type(stb_lexer *l, const char *filename, TypeExpr *type) {
  // TODO: support more internal type and user defined types;
  if (!expect_ids(l, filename, "void", 
                  "i8", "i16", "i32", "i64",
                  "u8", "u16", "u32", "u64")) { return false; }

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

bool compile_type_fn(stb_lexer *l, const char *filename, TypeExpr *type);

bool compile_type_expr(stb_lexer *l, const char *filename, TypeExpr *type)
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

    type->kind = TYPE_PTR;
    type->ref_type = malloc(sizeof(TypeExpr));
    *type->ref_type = ref_type;
    return true;
  }
  default:
    size_t mark = temp_save(); {
      pcompile_info(l, filename, "error: expected a type but got %s\n", token_name(l->token));
    } temp_rewind(mark);
    return false;
  }
}

bool compile_type_fn(stb_lexer *l, const char *filename, TypeExpr *type)
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


bool compile_local_var(stb_lexer *l, const char *filename, Program *prog, Fn *fn)
{
  bool result;

  Var var = { 0 };
  if (!prefetch_expect_token(l, filename, CLEX_id)) return_defer(false);
  var.name = strdup(l->string);
  if (!prefetch_not_none(l, filename)) return_defer(false);
  da_append(&fn->local, var);

  if (l->token == ':') {
    if (!prefetch_not_none(l, filename)) return_defer(false);
    if (!compile_type_expr(l, filename, &var.type)) return_defer(false);
    if (var.type.kind == TYPE_VOID) {
      pcompile_info(l, filename, "error: the type of a local variable cannot be \"void\"");
      return_defer(false);
    }
    if (!prefetch_not_none(l, filename)) return_defer(false);
  }
  
  if (l->token == '=') {
    Arg val;
    if (!prefetch_not_none(l, filename)) return_defer(false);
    if (!compile_arg(l, filename, prog, fn, &val)) return_defer(false);
    
    Op set_var = {
      .kind = OP_SET_VAR,
      .val = val,
    };
    get_local_var(&fn->local, &set_var.var, var.name);
    da_append(&fn->fn_body, set_var);
    if (!prefetch_not_none(l, filename)) return_defer(false);

    switch (val.kind) {
    case ARG_LIT_INT: var.type = (TypeExpr) {
        .kind = TYPE_INT,
        .size = 4,
      };
      break;
    default: TODO("auto detect more type during variable declaration");
    }
  }
  
  return_defer(true);
  
 defer:
  if (!result && var.name != NULL) free(var.name);
  return result;
}

bool compile_fn_body(stb_lexer *l, const char *filename, Program *prog, Fn *fn) {
  assert(l->token == '{');

  bool returned = false;
  if (!prefetch_not_none(l, filename)) return false;
  while (l->token != '}') {
    if (l->token == ';') {
      if (!prefetch_not_none(l, filename)) return false;
    } else if (l->token == CLEX_id && strcmp(l->string, "return") == 0) {
      Op ret = { .kind = OP_RETURN };
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
    Op ret = { .kind = OP_RETURN };
    da_append(&fn->fn_body, ret);
  }

  return true;
}

bool compile_function(stb_lexer *l, const char *filename, Program *prog)
{
  assert(expect_token(l, filename, CLEX_id) && strcmp(l->string, "fn") == 0);

  if (!prefetch_expect_token(l, filename, CLEX_id)) return false;

  if (symbol_defined(prog, l->string)) {
    pcompile_info(l, filename, "error: symbol %s redefined\n", l->string);
    return false;
  }

  Fn fn = {
    .name = strdup(l->string),
  };
  
  if (!prefetch_expect_token(l, filename, '(')) return false;
  if (!prefetch_expect_token(l, filename, ')')) return false;
  
  if (!prefetch_expect_token(l, filename, ':')) return false;
  if (!prefetch_not_none(l, filename)) return false;
  if (!compile_type_expr(l, filename, &fn.ret_type)) return false;
  
  if (!prefetch_expect_token(l, filename, '{')) return false;

  if (!compile_fn_body(l, filename, prog, &fn)) return false;
  
  if (!expect_token(l, filename, '}')) return false;

  da_append(&prog->fn_list, fn);

  return true;
}

void destroy_ext(Extern *ext)
{
  if (ext->name != NULL) free(ext->name);
  destroy_type_expr(&ext->type);
}

bool compile_file(stb_lexer *l, const char *filename, Program *prog)
{
  while (next_token(l, filename)) {
    if (l->token == ';') {
      continue;
    }
    
    if (!expect_ids(l, filename, "fn", "extern")) return false;
    
    if (strcmp(l->string, "fn") == 0) {
      if (!compile_function(l, filename, prog)) return false;
    } else if (strcmp(l->string, "extern") == 0) {
      Extern ext = {0};
      if (!prefetch_expect_token(l, filename, CLEX_id)) return false;
      ext.name = strdup(l->string);
      if (!prefetch_expect_token(l, filename, ':')) return false;
      if (!prefetch_not_none(l, filename)) return false;
      if (!compile_type_expr(l, filename, &ext.type)) return false;

      da_append(&prog->externs, ext);
    } else {
      UNREACHABLE(temp_sprintf("unexpected id: %s", l->string));
    }
  }

  return true;
}

void destroy_arg(Arg *arg)
{
  switch(arg->kind) {
  case ARG_NONE: break;
  case ARG_NAME: free(arg->name); break;
  case ARG_VAR_LOC: break;
  case ARG_LIT_INT: break;
  case ARG_LIT_STR: break;
  default: UNREACHABLE("destroy arg");
  }
}

void destroy_op(Op *op)
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

void destroy_fn_list(FnList *fn_list)
{
  da_foreach (Fn, fn, fn_list) {
    free(fn->name);
    destroy_type_expr(&fn->ret_type);
    
    da_foreach (Op, op, &fn->fn_body) {
      destroy_op(op);
    }
    da_free(fn->fn_body);
    
    da_foreach (Var, var, &fn->local) {
      free(var->name);
      destroy_type_expr(&var->type);
    }
    da_free(fn->local);
  }
  da_free(*fn_list);
}

void destroy_program(Program *prog)
{
  da_foreach (Extern, ext, &prog->externs) {
    free(ext->name);
    destroy_type_expr(&ext->type);
  }
  da_free(prog->externs);

  destroy_fn_list(&prog->fn_list);
  if (prog->str_lits.capacity > 0) {
    da_foreach (char *, str, &prog->str_lits) {
      free(*str);
    }
    da_free(prog->str_lits);
  }
}
