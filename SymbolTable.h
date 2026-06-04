// this is a temporary header
// and will removed after the paser is fully refactored to ast

#ifndef MCC_SYMBOL_TABLE_H_
#define MCC_SYMBOL_TABLE_H_

#include "3rd/nob.h"
#include "3rd/ht.h"

#include "lexer.h"
#include "type.h"

typedef enum {
  SYMBOL_FN = 0,
  SYMBOL_VAR,
  SYMBOL_EXTERN,
  __symbol_kind_count,
} SymbolKind;

typedef struct Var Var;
typedef struct Extern Extern;
typedef struct Fn Fn;
// typedef struct AST_Fn AST_Fn;

typedef struct {
  SymbolKind kind;
  Cursor loc;
  union {
    Var *var;
    Extern *ext;
    // AST_Fn *ast_fn;
    Fn *fn;
    void *ptr;
  };
} Symbol;

typedef struct {
  Extern **items;
  size_t count;
  size_t capacity;
} ExternList;

struct Var {
  TypeExpr type;
  size_t id;

  ptrdiff_t offset;
};

typedef struct {
  Var **items;
  size_t memsize;

  size_t count;
  size_t capacity;
} VarList;

typedef struct Scoop Scoop;
struct Scoop {
  Scoop *upper;
  Ht(String_View, Symbol) symbols;
};

Symbol *insert_sym(Scoop *sp, String_View name, Cursor loc, SymbolKind kind);
String_View sym_name(Scoop* sp, void *sym);
Scoop *new_scoop(Scoop *upper);

// C is so bad
typedef struct {
  Scoop *scoop;
  Symbol *sym;
} SymSearchResult;

SymSearchResult sym_search(Scoop *sp, String_View name);

#endif // MCC_SYMBOL_TABLE_H_

#ifdef MCC_SYMBOL_TABLE_IMPELEMTATION

Scoop *new_scoop(Scoop *upper)
{
  Scoop *s = calloc(1, sizeof(*s));
  s->symbols.hasheq = ht_sv_hasheq;
  s->upper = upper;
  return s;
}

Symbol *insert_sym(Scoop *sp, String_View name, Cursor loc, SymbolKind kind)
{
  Symbol *sym = ht_find(&sp->symbols, name);
  if (sym != NULL) {
    pcompile_info(loc,
                  "error: symbol "SV_Fmt" redefined in this scoop\n",
                  SV_Arg(name));
    // TODO: report where the symbol is first defined;
    return NULL;
  } else {
    sym = ht_put(&sp->symbols, name);
    *sym = (Symbol) {
      .kind = kind,
      .loc = loc,
    };
    return sym;
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

#endif // MCC_SYMBOL_TABLE_IMPELEMTATION

