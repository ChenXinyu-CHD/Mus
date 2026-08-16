#ifndef MCC_TYPE_H_
#define MCC_TYPE_H_

typedef enum {
  TYPE_UNKNOWN = 0,
  TYPE_VOID,
  TYPE_INT,
  TYPE_UINT,
  TYPE_BOOL,
  TYPE_FN,
  TYPE_PTR,
  TYPE_ARRAY,
  __type_kind_count,
} TypeKind;

typedef struct TypeExpr TypeExpr;

typedef struct {
  TypeExpr *items;
  size_t count;
  size_t capacity;
} TypeList;

typedef struct {
  TypeExpr *ret;
  TypeList args;
  bool va_args;
} FnType;

typedef struct {
  TypeExpr *inner;
  bool mutable;
} PtrType;

typedef struct {
  TypeExpr *inner;
  size_t count;
} ArrType;

struct TypeExpr {
  TypeKind kind;
  size_t size;

  union {
    PtrType ptr;
    FnType fn;
    ArrType arr;
  };
};

#define type_bool()    ((TypeExpr) {.kind = TYPE_BOOL,    .size = 1})
#define type_unknown() ((TypeExpr) {.kind = TYPE_UNKNOWN, .size = 0})
#define type_void()    ((TypeExpr) {.kind = TYPE_VOID,    .size = 0})
#define type_int(k, s) ((TypeExpr) {.kind = k,            .size = s})
TypeExpr type_fn(TypeExpr ret_type, TypeList arg_types, bool va_args);
TypeExpr type_ptr(TypeExpr inner, bool mutable);

void dump_type_expr(TypeExpr *type, FILE *stream);
// return true if lhs is exactly equals to rhs.
bool type_eq(const TypeExpr *lhs, const TypeExpr *rhs);

void destroy_type_expr(TypeExpr* type);

#endif // MCC_TYPE_H_

#ifdef MCC_TYPE_IMPLEMENTATION

#include "3rd_wrapper.h"

TypeExpr type_ptr(TypeExpr inner, bool mutable)
{
  TypeExpr type = {
    .kind = TYPE_PTR,
    .size = 8,
    .ptr = {
      .inner = arena_alloc(sizeof(TypeExpr)),
      .mutable = mutable,
    }
  };
  *type.ptr.inner = inner;

  return type;
}

TypeExpr type_fn(TypeExpr ret_type, TypeList arg_types, bool va_args)
{
  TypeExpr type = {
    .kind = TYPE_FN,
    .size = 8,
    .fn = {
      .ret = arena_alloc(sizeof(TypeExpr)),
      .args = arg_types,
      .va_args = va_args,
    },
  };
  *type.fn.ret = ret_type;

  return type;
}

bool type_eq(const TypeExpr *lhs, const TypeExpr *rhs)
{
  static_assert(__type_kind_count == 8, "introduced more type kinds");
  if (lhs->kind != rhs->kind || lhs->size != rhs->size) return false;

  switch (lhs->kind) {
  case TYPE_INT:
  case TYPE_UINT:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_UNKNOWN:
    return true;
  case TYPE_PTR:
    return lhs->ptr.mutable == rhs->ptr.mutable &&
      type_eq(lhs->ptr.inner, rhs->ptr.inner);
  case TYPE_ARRAY:
    return lhs->arr.count == rhs->arr.count &&
      type_eq(lhs->arr.inner, rhs->arr.inner);
    break;
  case TYPE_FN:
    if (lhs->fn.va_args != rhs->fn.va_args) return false;

    if (lhs->fn.args.count != rhs->fn.args.count) return false;
    for (size_t i = 0; i < lhs->fn.args.count; ++i) {
      TypeExpr *la = &lhs->fn.args.items[i];
      TypeExpr *ra = &rhs->fn.args.items[i];
      if (!type_eq(la, ra)) return false;
    }

    if (!type_eq(lhs->fn.ret, rhs->fn.ret)) return false;

    return true;
  default: UNREACHABLE("");
  }
}

void dump_type_expr(TypeExpr *type, FILE *stream)
{
  static_assert(__type_kind_count == 8, "introduced more type kinds");
  switch(type->kind) {
  case TYPE_UNKNOWN:
    fprintf(stream, "unknown type");
    break;
  case TYPE_VOID:
    fprintf(stream, "void");
    break;
  case TYPE_BOOL:
    fprintf(stream, "bool");
    break;
  case TYPE_INT:
    fprintf(stream, "i%ld", type->size * 8);
    break;
  case TYPE_UINT:
    fprintf(stream, "u%ld", type->size * 8);
    break;
  case TYPE_FN:
    fprintf(stream, "fn(");
    for (size_t i = 0; i < type->fn.args.count; ++i) {
      dump_type_expr(&type->fn.args.items[i], stream);
      if (i + 1 < type->fn.args.count) {
        fprintf(stream, ",");
      } else if (type->fn.va_args) {
        fprintf(stream, ",...");
      }
    }
    fprintf(stream, ")->");
    dump_type_expr(type->fn.ret, stream);
    break;
  case TYPE_PTR:
    fprintf(stream, "&");
    if (type->ptr.mutable) fprintf(stream, "mut ");
    dump_type_expr(type->ptr.inner, stream);
    break;
  case TYPE_ARRAY:
    fprintf(stream, "[");
    dump_type_expr(type->arr.inner, stream);
    fprintf(stream, ";%ld]", type->arr.count);
    break;
  default: UNREACHABLE("type");
  }
}

#endif // MCC_TYPE_IMPLEMENTATION
