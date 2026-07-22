#ifndef MCC_LEXER_H
#define MCC_LEXER_H

#include <stdio.h>

#include "3rd_wrapper.h"

int sv_to_int(String_View sv);

typedef struct {
  String_View filename;
  size_t row;
  size_t col;
  size_t stride;
} Cursor;

#define CS_Fmt SV_Fmt":%ld:%ld:"
#define CS_Arg(cs) SV_Arg((cs).filename), ((cs).row + 1), ((cs).col + 1)
#define cs_pos(cs) ((cs).col + (cs).stride)

void vpcompile_info(Cursor cs, const char* fmt, va_list args);
void pcompile_info(Cursor cs, const char* fmt, ...);

char cs_getc(String_Builder sb, Cursor c);
char cs_nextc(String_Builder sb, Cursor *c);
bool cs_move_after_prefix(String_Builder sb, Cursor *cs, String_View prefix);
String_View sv_between_cs(String_Builder sb, Cursor begin, Cursor end);

typedef enum {
  TOKEN_ID = 128,
  TOKEN_STR,
  TOKEN_INT,
  TOKEN_VAR,
  TOKEN_LET,
  TOKEN_FN,
  TOKEN_RET,
  TOKEN_EXT,
  TOKEN_ERR,
  TOKEN_DOTS,
  TOKEN_BOOL,
  TOKEN_TRUE,
  TOKEN_FALSE,
  TOKEN_IF,
  TOKEN_ELSE,
  TOKEN_EOF,

  // operators
  TOKEN_EQ,
  TOKEN_NEQ,
  TOKEN_LE,
  TOKEN_GE,
  TOKEN_ARR,

  // internal types
  TOKEN_U8,
  TOKEN_U16,
  TOKEN_U32,
  TOKEN_U64,
  TOKEN_I8,
  TOKEN_I16,
  TOKEN_I32,
  TOKEN_I64,
  TOKEN_VOID,
  __token_kind_count,
} TokenKind;

typedef struct {
  int kind;
  String_View str;
  Cursor start;
} Token;

const char *token_name(int kind);

typedef struct {
  String_Builder src;

  Token current;
  Cursor cursor;
} Lexer;

bool lexer_init(Lexer *lexer, String_View filename);
bool lexer_next(Lexer *lexer);

bool expect_token(Lexer *l, int token);
bool vexpect_tokens_impl(Lexer *l, va_list expecteds);
bool expect_tokens_impl(Lexer *l, ...);
#define expect_tokens(l, ...)                          \
  expect_tokens_impl(l, __VA_ARGS__, TOKEN_ERR)

bool prefetch_not_none(Lexer *l);

bool prefetch_expect_token(Lexer *l, int token);
bool prefetch_expect_tokens_impl(Lexer *l, ...);
#define prefetch_expect_tokens(l, ...)                          \
  prefetch_expect_tokens_impl(l, __VA_ARGS__, TOKEN_ERR)

void lexer_terminate(Lexer *lexer);

#endif // MCC_LEXER_H

#ifdef MCC_LEXER_IMPLEMENTATION

int sv_to_int(String_View sv)
{
  int val = 0;
  for (size_t i = 0; i < sv.count; ++i) {
    assert(isdigit(sv.data[i]));
    val *= 10;
    val += sv.data[i] - '0';
  }
  return val;
}

void vpcompile_info(Cursor cs, const char* fmt, va_list args)
{
  size_t mark = temp_save(); {
    va_list ap;
    va_copy(ap, args);
    char *msg = temp_vsprintf(fmt, ap);
    va_end(ap);

    fprintf(stderr, CS_Fmt" %s", CS_Arg(cs), msg);
  } temp_rewind(mark);
}

void pcompile_info(Cursor cs, const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vpcompile_info(cs, fmt, args);
  va_end(args);
}


char cs_getc(String_Builder sb, Cursor c)
{
  size_t pos = cs_pos(c);
  if (pos >= sb.count) return '\0';
  return sb.items[pos];
}

char cs_nextc(String_Builder sb, Cursor *c)
{
  assert(c != NULL);
  char ch = cs_getc(sb, *c);
  if (ch == '\0') return '\0';

  c->col += 1;
  if (ch == '\n') {
    c->row += 1;
    c->stride += c->col;
    c->col = 0;
  }
  return cs_getc(sb, *c);
}

bool cs_move_after_prefix(String_Builder sb, Cursor *cs, String_View prefix)
{
  Cursor fallback = *cs;

  size_t i = 0;
  while (i < prefix.count) {
    if (cs_getc(sb, *cs) != prefix.data[i]) {
      break;
    }
    cs_nextc(sb, cs);
    i += 1;
  }

  if (i < prefix.count) {
    *cs = fallback;
    return false;
  }
  return true;
}

String_View sv_between_cs(String_Builder sb, Cursor begin, Cursor end)
{
  assert(cs_pos(begin) < sb.count);
  assert(cs_pos(end) <= sb.count);
  return (String_View) {
    .data = sb.items + cs_pos(begin),
    .count = cs_pos(end) - cs_pos(begin),
  };
}

static_assert(__token_kind_count == 128 + 30, "introduced more token kinds");
static struct {
  int kind;
  char *str;
  char *print_name;
} token_list[] = {
  {.str = "@extern", .print_name = "@extern", .kind = TOKEN_EXT},
  {.str = "fn",      .print_name = "fn",      .kind = TOKEN_FN},
  {.str = "return",  .print_name = "return",  .kind = TOKEN_RET},
  {.str = "var",     .print_name = "var",     .kind = TOKEN_VAR},
  {.str = "let",     .print_name = "let",     .kind = TOKEN_LET},
  {.str = "if",      .print_name = "if",      .kind = TOKEN_IF},
  {.str = "else",    .print_name = "else",    .kind = TOKEN_ELSE},
  {.str = "true",    .print_name = "true",    .kind = TOKEN_TRUE},
  {.str = "false",   .print_name = "false",   .kind = TOKEN_FALSE},
  // multi-charactor operators
  {.str = "==",  .print_name = "'=='",  .kind = TOKEN_EQ},
  {.str = "!=",  .print_name = "'!='",  .kind = TOKEN_NEQ},
  {.str = "<=",  .print_name = "'<='",  .kind = TOKEN_LE},
  {.str = ">=",  .print_name = "'>='",  .kind = TOKEN_GE},
  {.str = "...", .print_name = "'...'", .kind = TOKEN_DOTS},
  {.str = "->",  .print_name = "'->'",  .kind = TOKEN_ARR},
  // ascii
  {.str = "(", .print_name = "'('", .kind = '('},
  {.str = ")", .print_name = "')'", .kind = ')'},
  {.str = ":", .print_name = "':'", .kind = ':'},
  {.str = "{", .print_name = "'{'", .kind = '{'},
  {.str = "}", .print_name = "'}'", .kind = '}'},
  {.str = ";", .print_name = "';'", .kind = ';'},
  {.str = ",", .print_name = "','", .kind = ','},
  {.str = "=", .print_name = "'='", .kind = '='},
  {.str = "&", .print_name = "'&'", .kind = '&'},
  {.str = "+", .print_name = "'+'", .kind = '+'},
  {.str = "-", .print_name = "'-'", .kind = '-'},
  {.str = "*", .print_name = "'*'", .kind = '*'},
  {.str = "/", .print_name = "'/'", .kind = '/'},
  {.str = "%", .print_name = "'%'", .kind = '%'},
  {.str = "<", .print_name = "'<'", .kind = '<'},
  {.str = ">", .print_name = "'>'", .kind = '>'},
  // internal types
  {.str = "bool", .print_name = "bool", .kind = TOKEN_BOOL},
  {.str = "u8",   .print_name = "u8",   .kind = TOKEN_U8},
  {.str = "u16",  .print_name = "u16",  .kind = TOKEN_U16},
  {.str = "u32",  .print_name = "u32",  .kind = TOKEN_U32},
  {.str = "u64",  .print_name = "u64",  .kind = TOKEN_U64},
  {.str = "i8",   .print_name = "i8",   .kind = TOKEN_I8},
  {.str = "i16",  .print_name = "i16",  .kind = TOKEN_I16},
  {.str = "i32",  .print_name = "i32",  .kind = TOKEN_I32},
  {.str = "i64",  .print_name = "i64",  .kind = TOKEN_I64},
  {.str = "void", .print_name = "void", .kind = TOKEN_VOID},
  // complex tokens, their str is uncertain
  {.print_name = "string literal",  .kind = TOKEN_STR},
  {.print_name = "integer literal", .kind = TOKEN_INT},
  {.print_name = "identifier", .kind = TOKEN_ID},
  // special token, they have no str part
  {.print_name = "EOF",       .kind = TOKEN_EOF},
  {.print_name = "Err token", .kind = TOKEN_ERR},
};

const char *token_name(int kind)
{
  for (size_t i = 0; i < ARRAY_LEN(token_list); ++i) {
    if (token_list[i].kind == kind) {
      return token_list[i].print_name;
    }
  }

  UNREACHABLE("");
}

bool lexer_init(Lexer *lexer, String_View filename)
{
  lexer->src.count = 0;
  if (!read_entire_file(filename.data, &lexer->src)) {
    fprintf(stderr, "lexical error: can not read file "SV_Fmt"\n", SV_Arg(filename));
    return false;
  }

  lexer->current = (Token) {0};
  lexer->cursor = (Cursor) {
    .filename = filename,
    .row = 0,
    .col = 0,
    .stride = 0,
  };

  return true;
}

static Cursor skip_useless(String_Builder sb, Cursor c)
{
  while (isspace(cs_getc(sb, c)) || cs_getc(sb, c) == '#') {
    // white spaces
    while (isspace(cs_getc(sb, c))) {
      cs_nextc(sb, &c);
    }
    // commits
    if (cs_getc(sb, c) == '#') {
      while (cs_getc(sb, c) != '\0' && cs_getc(sb, c) != '\n') {
        cs_nextc(sb, &c);
      }
    }
  }

  return c;
}

static bool lexer_number(Lexer *l)
{
  // TODO: support float type
  Cursor start = l->cursor;
  Cursor end = start;
  String_Builder sb = l->src;

  assert(isdigit(cs_getc(sb, end)));
  while (isdigit(cs_getc(sb, end))) {
    cs_nextc(sb, &end);
  }

  String_View str = sv_between_cs(sb, start, end);
  l->current = (Token) {
    .kind = TOKEN_INT,
    .str = str,
    .start = start,
  };
  l->cursor = end;
  return true;
}

static bool lexer_simple_token(Lexer *l)
{
  String_Builder sb = l->src;
  Cursor start = l->cursor;

  for (size_t i = 0; i < ARRAY_LEN(token_list); ++i) {
    if (token_list[i].str == NULL) continue;

    String_View str = sv_from_cstr(token_list[i].str);
    if (cs_move_after_prefix(sb, &l->cursor, str)) {
      l->current = (Token) {
        .kind = token_list[i].kind,
        .str = str,
        .start = start,
      };
      return true;
    }
  }
  return false;
}

static bool lexer_id_keyword(Lexer *l)
{
  Cursor start = l->cursor;
  Cursor end = start;
  String_Builder sb = l->src;

  assert(isalpha(cs_getc(sb, end)));
  while (isalnum(cs_getc(sb, end)) || cs_getc(sb, end) == '_') {
    cs_nextc(sb, &end);
  }
  String_View str = sv_between_cs(sb, start, end);
  l->current = (Token) {
    .kind = TOKEN_ID,
    .str = str,
    .start = start,
  };
  l->cursor = end;
  return true;
}

static bool lexer_str_literal(Lexer *l)
{
  String_Builder sb = l->src;
  Cursor start = l->cursor;
  Cursor end = start;
  assert(cs_getc(sb, end) == '"');

  cs_nextc(sb, &end);
  while (cs_getc(sb, end) != '\0' &&
         cs_getc(sb, end) != '\n' &&
         cs_getc(sb, end) != '"') {
    if (cs_getc(sb, end) == '\\') {
      cs_nextc(sb, &end);
    }
    cs_nextc(sb, &end);
  }

  String_View str = sv_between_cs(sb, start, end);
  if (cs_getc(sb, end) != '"') {
    l->current.kind = TOKEN_ERR;
    l->current.str = str;
  } else {
    cs_nextc(sb, &end);
    sv_chop_prefix(&str, sv_from_cstr("\""));

    l->current.kind = TOKEN_STR;
    l->current.str = str;
  }
  l->cursor = end;

  return l->current.kind == TOKEN_STR;
}

bool lexer_next(Lexer *lexer)
{
  lexer->cursor = skip_useless(lexer->src, lexer->cursor);
  char ch = cs_getc(lexer->src, lexer->cursor);
  if (ch == '\0') {
    lexer->current = (Token) {
      .start = lexer->cursor,
      .kind = TOKEN_EOF
    };
    return false;
  } if (lexer_simple_token(lexer)) {
    return true;
  } else if (isdigit(ch)) {
    return lexer_number(lexer);
  } else if (isalpha(ch)) {
    return lexer_id_keyword(lexer);
  } else if (ch == '"'){
    return lexer_str_literal(lexer);
  } else {
    lexer->current = (Token) {
      .start = lexer->cursor,
      .kind = TOKEN_ERR,
    };
    pcompile_info(lexer->cursor, "error: lexer error\n");
    return false;
  }
}

bool prefetch_not_none(Lexer *l)
{
  bool result = lexer_next(l);
  if (l->current.kind == TOKEN_EOF) {
    pcompile_info(l->current.start, "error: unexpected EOF\n");
    return false;
  }

  return result;
}

bool expect_token(Lexer *l, int token)
{
  if (l->current.kind == token) return true;

  pcompile_info(l->current.start,
                "error: expect token %s, but got %s\n",
                token_name(token),
                token_name(l->current.kind));
  return false;
}

bool prefetch_expect_token(Lexer *l, int token)
{
  lexer_next(l);
  if (!expect_token(l, token)) return false;
  return true;
}

bool vexpect_tokens_impl(Lexer *l, va_list expecteds)
{
  bool found = false;

  va_list ap; va_copy(ap, expecteds); {
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

  va_copy(ap, expecteds); {
    pcompile_info(l->current.start, "error: expect token ");

    int arg = va_arg(ap, int);
    while (arg != TOKEN_ERR) {
      fprintf(stderr, "%s, ", token_name(arg));
      arg = va_arg(ap, int);
    }
    fprintf(stderr, "but got %s\n", token_name(l->current.kind));
  } va_end(ap);

  return false;
}

bool expect_tokens_impl(Lexer *l, ...)
{
  bool found = false;

  va_list ap; va_start(ap, l); {
    found = vexpect_tokens_impl(l, ap);
  } va_end(ap);

  return found;
}

bool prefetch_expect_tokens_impl(Lexer *l, ...)
{
  lexer_next(l);
  bool found = false;

  va_list ap; va_start(ap, l); {
    found = vexpect_tokens_impl(l, ap);
  } va_end(ap);

  return found;
}

void lexer_terminate(Lexer *lexer)
{
  if (lexer->src.capacity != 0) da_free(lexer->src);
}

#endif // MCC_LEXER_IMPLEMENTATION
