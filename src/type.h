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
  __type_kind_count,
} TypeKind;

typedef struct TypeExpr TypeExpr;

typedef struct {
  TypeExpr *items;
  size_t count;
  size_t capacity;
} TypeList;

typedef struct {
  TypeExpr *ret_type;
  TypeList arg_types;
  bool va_args;
} FnType;

struct TypeExpr {
  TypeKind kind;
  size_t size;

  union {
    TypeExpr *ref_type;
    FnType fn_type;
  };
};

TypeExpr type_bool();
TypeExpr type_fn(TypeExpr ret_type, TypeList arg_types, bool va_args);
TypeExpr type_ptr(TypeExpr inner);

TypeExpr type_clone(TypeExpr t);

void dump_type_expr(TypeExpr *type, FILE *stream);
// return true if lhs is exactly equals to rhs.
bool type_eq(const TypeExpr *lhs, TypeExpr *rhs);
bool type_matched(TypeExpr *required, TypeExpr *actual);

void destroy_type_expr(TypeExpr* type);

#endif // MCC_TYPE_H_

#ifdef MCC_TYPE_IMPLEMENTATION

#include "3rd_wrapper.h"

TypeExpr type_bool()
{
  return (TypeExpr) {.kind = TYPE_BOOL, .size = 1};
}

TypeExpr type_ptr(TypeExpr inner)
{
  TypeExpr type = {
    .kind = TYPE_PTR,
    .size = 8,
    .ref_type = arena_alloc(sizeof(TypeExpr))
  };
  *type.ref_type = inner;

  return type;
}

TypeExpr type_fn(TypeExpr ret_type, TypeList arg_types, bool va_args)
{
  TypeExpr type = {
    .kind = TYPE_FN,
    .size = 8,
    .fn_type = {
      .ret_type = arena_alloc(sizeof(TypeExpr)),
      .arg_types = arg_types,
      .va_args = va_args,
    },
  };
  *type.ref_type = ret_type;

  return type;
}

TypeExpr type_clone(TypeExpr t)
{
  TypeExpr result = {0};
  static_assert(__type_kind_count == 7, "introduced more type kinds");
  switch (t.kind) {
  case TYPE_UNKNOWN:
  case TYPE_BOOL:
  case TYPE_VOID:
  case TYPE_INT:
  case TYPE_UINT:
    result = t;
    break;
  case TYPE_PTR:
    result = type_ptr(type_clone(*t.ref_type));
    break;
  case TYPE_FN: {
    TypeList arg_types = {0};
    da_foreach (TypeExpr, arg, &t.fn_type.arg_types) {
      da_append(&arg_types, type_clone(*arg));
    }
    result = type_fn(*t.fn_type.ret_type, arg_types, t.fn_type.va_args);
  }break;
  default: UNREACHABLE("type_clone");
  }

  return result;
}

bool type_matched(TypeExpr *required, TypeExpr *actual)
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

bool type_eq(const TypeExpr *lhs, TypeExpr *rhs)
{
  static_assert(__type_kind_count == 7, "introduced more type kinds");
  if (lhs->kind != rhs->kind || lhs->size != rhs->size) return false;

  switch (lhs->kind) {
  case TYPE_INT:
  case TYPE_UINT:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_UNKNOWN:
    return true;
  case TYPE_PTR:
    return type_eq(lhs->ref_type, rhs->ref_type);
  case TYPE_FN:
    if (lhs->fn_type.va_args != rhs->fn_type.va_args) return false;

    if (lhs->fn_type.arg_types.count != rhs->fn_type.arg_types.count) return false;
    for (size_t i = 0; i < lhs->fn_type.arg_types.count; ++i) {
      TypeExpr *la = &lhs->fn_type.arg_types.items[i];
      TypeExpr *ra = &rhs->fn_type.arg_types.items[i];
      if (!type_eq(la, ra)) return false;
    }

    if (!type_eq(lhs->fn_type.ret_type, rhs->fn_type.ret_type)) return false;

    return true;
  default: UNREACHABLE("");
  }
}

void dump_type_expr(TypeExpr *type, FILE *stream)
{
  static_assert(__type_kind_count == 7, "introduced more type kinds");
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

void destroy_type_expr(TypeExpr* type)
{
  if (type == NULL) return;

  static_assert(__type_kind_count == 7, "introduced more type kinds");
  switch(type->kind) {
  case TYPE_INT: break;
  case TYPE_UINT: break;
  case TYPE_BOOL: break;
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

#endif // MCC_TYPE_IMPLEMENTATION
