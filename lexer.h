#ifndef MCC_LEXER_H
#define MCC_LEXER_H

#include <stdio.h>

#include "nob.h"

typedef struct {
  String_View filename;
  size_t row;
  size_t col;
  size_t stride;
} Cursor;

#define CS_Fmt SV_Fmt":%ld:%ld:"
#define CS_Arg(cs) SV_Arg((cs).filename), ((cs).row + 1), ((cs).col + 1)
#define cs_pos(cs) ((cs).col + (cs).stride)

char cs_getc(String_Builder sb, Cursor c);
char cs_nextc(String_Builder sb, Cursor *c);
bool cs_move_after_prefix(String_Builder sb, Cursor *cs, String_View prefix);
String_View sv_between_cs(String_Builder sb, Cursor begin, Cursor end);

typedef enum {
  TOKEN_ID = 128,
  TOKEN_STR,
  TOKEN_INT,
  TOKEN_VAR,
  TOKEN_FN,
  TOKEN_RET,
  TOKEN_EXT,
  TOKEN_ERR,
  TOKEN_DOTS,
  TOKEN_EOF,
} TokenKind;

#define TOKEN_KIND_COUNT 10
void dump_token_kind(FILE *stream, int kind);

typedef struct {
  int kind;
  String_View token;
  Cursor start;
} Token;

typedef struct {
  String_Builder src;

  Token current;
  Cursor cursor;
} Lexer;

bool lexer_init(Lexer *lexer, String_View filename);
bool lexer_next(Lexer *lexer);
void lexer_terminate(Lexer *lexer);

#endif // MCC_LEXER_H

#ifdef MCC_LEXER_IMPLEMENTATION

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

void dump_token_kind(FILE *stream, int kind)
{
  static_assert(TOKEN_KIND_COUNT == 10, "introduced more token kind");
  switch (kind) {
  case TOKEN_ID:
    fprintf(stream, "id"); break;
  case TOKEN_STR:
    fprintf(stream, "string literal"); break;
  case TOKEN_INT:
    fprintf(stream, "int"); break;
  case TOKEN_VAR:
    fprintf(stream, "var"); break;
  case TOKEN_FN:
    fprintf(stream, "fn"); break;
  case TOKEN_RET:
    fprintf(stream, "return"); break;
  case TOKEN_EXT:
    fprintf(stream, "extern"); break;
  case TOKEN_ERR:
    fprintf(stream, "err"); break;
  case TOKEN_DOTS:
    fprintf(stream, "..."); break;
  case TOKEN_EOF:
    fprintf(stream, "eof"); break;
  default:
    assert(kind > 0 && kind <= 127 && "only ascii code can be a token");
    fputc(kind, stream);
  }
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

  String_View token = sv_between_cs(sb, start, end);
  l->current = (Token) {
    .kind = TOKEN_INT,
    .token = token,
    .start = start,
  };
  l->cursor = end;
  return true;
}

static_assert(TOKEN_KIND_COUNT == 10);

static struct {
  char *token;
  int kind;
} simple_tokens[] = {
  {.token = "extern", .kind = TOKEN_EXT},
  {.token = "fn", .kind = TOKEN_FN},
  {.token = "return", .kind = TOKEN_RET},
  {.token = "var", .kind = TOKEN_VAR},
  {.token = "...", .kind = TOKEN_DOTS},
  {.token = "(", .kind = '('},
  {.token = ")", .kind = ')'},
  {.token = ":", .kind = ':'},
  {.token = "{", .kind = '{'},
  {.token = "}", .kind = '}'},
  {.token = ";", .kind = ';'},
  {.token = ",", .kind = ','},
  {.token = "=", .kind = '='},
  {.token = "&", .kind = '&'},
  {.token = ".", .kind = '.'},
};

static bool lexer_simple_token(Lexer *l)
{
  String_Builder sb = l->src;
  Cursor start = l->cursor;

  for (size_t i = 0; i < ARRAY_LEN(simple_tokens); ++i) {
    String_View token = sv_from_cstr(simple_tokens[i].token);
    if (cs_move_after_prefix(sb, &l->cursor, token)) {
      l->current = (Token) {
        .kind = simple_tokens[i].kind,
        .token = token,
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
  String_View token = sv_between_cs(sb, start, end);
  l->current = (Token) {
    .kind = TOKEN_ID,
    .token = token,
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

  String_View token = sv_between_cs(sb, start, end);
  if (cs_getc(sb, end) != '"') {
    l->current.kind = TOKEN_ERR;
    l->current.token = token;
  } else {
    cs_nextc(sb, &end);
    sv_chop_prefix(&token, sv_from_cstr("\""));

    l->current.kind = TOKEN_STR;
    l->current.token = token;
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
    return false;
  }
}

void lexer_terminate(Lexer *lexer)
{
  if (lexer->src.capacity != 0) da_free(lexer->src);
}

#endif // MCC_LEXER_IMPLEMENTATION
