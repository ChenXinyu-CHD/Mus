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


bool symbol_defined(SymbolTable *syms, char *name, size_t begin)
{
  for (size_t i = begin; i < syms->count; ++i) {
    if (strcmp(syms->items[0].name, name) == 0) {
      return true;
    }
  }

  return false;
}

bool compile_arg(stb_lexer *l, const char *filename, SymbolTable *syms, Arg *arg)
{
  UNUSED(syms);
  assert(l->token != CLEX_eof);
  switch (l->token) {
  case CLEX_id: {
    arg->kind = ARG_NAME;
    arg->name = strdup(l->string);
    break;
  }
  case CLEX_sqstring:
    TODO("CLEX_sqstring");
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

bool compile_expr(stb_lexer *l, const char *filename, SymbolTable *syms, OpList *fn_body)
{
  Arg arg;
  if (!compile_arg(l, filename, syms, &arg)) return false;
  if (!prefetch_not_none(l, filename)) return false;

  if (l->token == '(') {
    Op invoke = { .kind = OP_INVOKE, .fn = arg };
    
    if (!prefetch_not_none(l, filename)) return false;
    while (l->token != ')') {
      if (!compile_arg(l, filename, syms, &arg)) return false;
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
    
    da_append(fn_body, invoke);
  } else {
    UNREACHABLE(token_name(l->token));
  }
  
  return true;
}

bool compile_fn_body(stb_lexer *l, const char *filename, SymbolTable *syms, OpList *fn_body) {
  assert(l->token == '{');
  
  if (!prefetch_not_none(l, filename)) return false;
  while (l->token != '}') {
    if (l->token == CLEX_id && strcmp(l->string, "return") == 0) {
      Op ret = { .kind = OP_RETURN };
      if (!prefetch_not_none(l, filename)) return false;
      if (!compile_arg(l, filename, syms, &ret.ret_val)) return false;
      da_append(fn_body, ret);
    } else {
      if (!compile_expr(l, filename, syms, fn_body)) return false;
    }
    if (!prefetch_not_none(l, filename)) return false;
  }

  if (fn_body->count == 0) {
      Op ret = { .kind = OP_RETURN };
      da_append(fn_body, ret);
  }

  return true;
}

bool compile_function(stb_lexer *l, const char *filename, SymbolTable *syms)
{
  assert(expect_token(l, filename, CLEX_id) && strcmp(l->string, "fn") == 0);

  if (!prefetch_expect_token(l, filename, CLEX_id)) return false;

  if (symbol_defined(syms, l->string, 0)) {
    pcompile_info(l, filename, "error: symbol %s redefined\n", l->string);
    return false;
  }
  char *name = strdup(l->string);
  
  if (!prefetch_expect_token(l, filename, '(')) return false;
  if (!prefetch_expect_token(l, filename, ')')) return false;
  
  if (!prefetch_expect_token(l, filename, '{')) return false;

  OpList fn_body = {0};
  if (!compile_fn_body(l, filename, syms, &fn_body)) return false;
  
  if (!expect_token(l, filename, '}')) return false;

  Symbol fn = {
    .name = name,
    .fn_body = fn_body,
  };
  da_append(syms, fn);

  return true;
}

bool compile_file(stb_lexer *l, const char *filename, SymbolTable *syms)
{
  while (next_token(l, filename)) {
    if (!expect_ids(l, filename, "fn", "extern")) return false;
    
    if (strcmp(l->string, "fn") == 0) {
      if (!compile_function(l, filename, syms)) return false;
    } else if (strcmp(l->string, "extern") == 0) {
      next_token(l, filename);
      Symbol sym = { .external = true };
      if (!expect_ids(l, filename, "fn")) return false;

      if (strcmp(l->string, "fn") == 0) {
        if (!prefetch_expect_token(l, filename, CLEX_id)) return false;

        if (symbol_defined(syms, l->string, 0)) {
          pcompile_info(l, filename, "error: symbol %s redefined", l->string);
        }
        sym.name = strdup(l->string);
        
        if (!prefetch_expect_token(l, filename, '(')) return false;
        if (!prefetch_expect_token(l, filename, ')')) return false;

        da_append(syms, sym);
      } else {
        UNREACHABLE("compile_file");
      }
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
  case ARG_LIT_INT: break;
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
  default: UNREACHABLE("destroy op");
  }
}

void destroy_symbol(Symbol *sym)
{
  free(sym->name);
  if (!sym->external) {
    da_foreach (Op, op, &sym->fn_body) {
      destroy_op(op);
    }
  }
}

void destroy_symtable(SymbolTable *syms)
{
  da_foreach (Symbol, sym, syms) {
    destroy_symbol(sym);
  }
  da_free(*syms);
}
