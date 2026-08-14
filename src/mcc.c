#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "3rd_wrapper.h"

#include "lexer.h"
#include "ir.h"

void append_str_lit(String_Builder *sb, String_View str)
{
  sb_appendf(sb, "\""SV_Fmt"\"", SV_Arg(str));
}

String_Builder gen_code_ir(const Program *prog)
{
  String_Builder sb = {0};

  for (size_t i = 0; i < prog->str_lits.count; ++i) {
    sb_appendf(&sb, ".S_%ld = ", i);
    append_str_lit(&sb, prog->str_lits.items[i]);
    sb_append(&sb, '\n');
  }

  sb_appendf(&sb, "\n");

  da_foreach(Fn *, fn, &prog->fn_list) {
    da_foreach(String_Builder, name, &(*fn)->names) {
      String_View fn_name = sb_to_sv(*name);
      sb_appendf(&sb, SV_Fmt":\n", SV_Arg(fn_name));
    }

    da_foreach (Op, op, &(*fn)->fn_body) {
      if (op->kind != OP_LABEL) {
        sb_appendf(&sb, "    ");
      }
      dump_op(&sb, op);
    }
  }

  return sb;
}

bool build_ir(const char *filename, const Program *prog)
{
  size_t mark = temp_save();

  String_Builder code = gen_code_ir(prog);
  filename = temp_sprintf("%s.ir", filename);
  bool success = write_entire_file(filename, code.items, code.count);
  da_free(code);

  temp_rewind(mark);
  return success;
}

typedef enum {
  RAX = 0,
  RBX,
  RCX,
  RDX,
  RSI,
  RDI,
  R8,
  R9,
  R10,
  R11,
  R12,
  R13,
  R14,
  R15,
  __x64_reg_count,
} X64RegKind;

static X64RegKind param_regs[] = {
  RDI, RSI, RDX, RCX, R8, R9
};

#define PARAM_REGS_CNT ARRAY_LEN(param_regs)

static const char *regs[][9] = {
  { [1] = "%al",   [2] = "%ax",   [4] = "%eax",  [8] = "%rax" },
  { [1] = "%bl",   [2] = "%bx",   [4] = "%ebx",  [8] = "%rbx" },
  { [1] = "%cl",   [2] = "%cx",   [4] = "%ecx",  [8] = "%rcx" },
  { [1] = "%dl",   [2] = "%dx",   [4] = "%edx",  [8] = "%rdx" },
  { [1] = "%sil",  [2] = "%si",   [4] = "%esi",  [8] = "%rsi" },
  { [1] = "%dil",  [2] = "%di",   [4] = "%edi",  [8] = "%rdi" },
  { [1] = "%r8b",  [2] = "%r8w",  [4] = "%r8d",  [8] = "%r8"  },
  { [1] = "%r9b",  [2] = "%r9w",  [4] = "%r9d",  [8] = "%r9"  },
  { [1] = "%r10b", [2] = "%r10w", [4] = "%r10d", [8] = "%r10" },
  { [1] = "%r11b", [2] = "%r11w", [4] = "%r11d", [8] = "%r11" },
  { [1] = "%r12b", [2] = "%r12w", [4] = "%r12d", [8] = "%r12" },
  { [1] = "%r13b", [2] = "%r13w", [4] = "%r13d", [8] = "%r13" },
  { [1] = "%r14b", [2] = "%r14w", [4] = "%r14d", [8] = "%r14" },
  { [1] = "%r15b", [2] = "%r15w", [4] = "%r15d", [8] = "%r15" },
};

static const char cmd_suff[9] = {
  [1] = 'b',
  [2] = 'w',
  [4] = 'l',
  [8] = 'q',
};

// currently, all regiters are spilled to memory
// TODO: implement a better method;
typedef struct {
  size_t size;
  int offset;
} X64VirtReg;

typedef struct {
  X64VirtReg *items;
  size_t count;
  size_t capacity;
  size_t memsize;
} X64RegMap;

X64RegMap x64_reg_alloc(const RegList regs)
{
  X64RegMap map = {0};
  map.memsize = 0;
  da_foreach(Reg, reg, &regs) {
    size_t size = reg->type.size;
    assert(size == 1 || size == 2 || size == 4 || size == 8);
    map.memsize = (map.memsize + size - 1) / size * size + size;
    X64VirtReg vreg = {
      .size = size,
      .offset = -map.memsize,
    };
    da_append(&map, vreg);
  }
  map.memsize = (map.memsize + 15) / 16 * 16 + 16;
  return map;
}

void X64_lit_to_vreg(String_Builder *sb, X64VirtReg vreg, int64_t lit)
{
  assert(vreg.size == 1 || vreg.size == 2 || vreg.size == 4 || vreg.size == 8);
  const char  s = cmd_suff[vreg.size];
  sb_appendf(sb, "    mov%c $%ld, %d(%%rbp)\n",
             s, lit, vreg.offset);
}

void X64_reg_to_vreg(String_Builder *sb, X64RegKind reg, X64VirtReg vreg)
{
  assert(vreg.size == 1 || vreg.size == 2 || vreg.size == 4 || vreg.size == 8);
  const char  s = cmd_suff[vreg.size];
  const char *r = regs[reg][vreg.size];
  sb_appendf(sb, "    mov%c %s, %d(%%rbp)\n",
             s, r, vreg.offset);
}

void X64_vreg_to_reg(String_Builder *sb, X64RegKind reg, X64VirtReg vreg)
{
  assert(vreg.size == 1 || vreg.size == 2 || vreg.size == 4 || vreg.size == 8);
  const char  s = cmd_suff[vreg.size];
  const char *r = regs[reg][vreg.size];
  sb_appendf(sb, "    mov%c %d(%%rbp), %s\n",
             s, vreg.offset, r);
}

static size_t arg2reg(String_Builder *sb, X64RegMap *map, Arg *arg, X64RegKind reg)
{
  size_t size = arg->type.size;
  assert(size <= 8);
  static_assert(__arg_kind_count == 7, "introduced more arg kinds");
  switch (arg->kind) {
  case ARG_REG:
    X64_vreg_to_reg(sb, reg, map->items[arg->reg.id]);
    break;
  case ARG_NONE:
    UNREACHABLE("the argument cannot be none");
    break;
  case ARG_GLOBAL_VAR: {
    sb_appendf(sb, "    leaq "SV_Fmt"@PLT(%%rip), %s\n",
               SV_Arg(arg->var->name), regs[reg][8]);
  } break;
  case ARG_FN: {
    assert(arg->fn->names.count != 0);
    String_View name = sb_to_sv(da_first(&arg->fn->names));
    sb_appendf(sb, "    leaq "SV_Fmt"@PLT(%%rip), %s\n",
               SV_Arg(name), regs[reg][size]);
  } break;
  case ARG_EXT: {
    sb_appendf(sb, "    leaq "SV_Fmt"@PLT(%%rip), %s\n",
               SV_Arg(arg->ext), regs[reg][size]);
  } break;
  case ARG_LIT_INT: {
    sb_appendf(sb, "    mov%c $%d, %s\n",
               cmd_suff[size],
               arg->num_int, regs[reg][size]);
  } break;
  case ARG_LIT_STR:
    sb_appendf(sb, "    leaq .S_%ld(%%rip), %s\n", arg->str_label, regs[reg][size]);
    break;
  default:
    UNREACHABLE("");
  }
  return size;
}

static void x64_store(String_Builder *sb, X64RegKind dst, X64RegKind src, size_t size)
{
  sb_appendf(sb, "    mov%c %s, (%s)\n",
             cmd_suff[size], regs[src][size], regs[dst][8]);
}

static void x64_load(String_Builder *sb, X64RegKind dst, X64RegKind src, size_t size)
{
  sb_appendf(sb, "    mov%c (%s), %s\n",
             cmd_suff[size], regs[src][8], regs[dst][size]);
}

// references:
// -- Instructions: https://www.felixcloutier.com/x86/
// -- x86 flags : https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/x86-architecture?source=recommendations#x86-flags
// -- regiters : https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/x64-architecture
String_Builder gen_code_x86_64_gas(const Program *prog)
{
  String_Builder sb = {0};

  sb_appendf(&sb, "    .section .rodata\n");
  for (size_t i = 0; i < prog->str_lits.count; ++i) {
    sb_appendf(&sb, ".S_%ld:\n", i);
    sb_appendf(&sb, "    .string ");
    append_str_lit(&sb, prog->str_lits.items[i]);
    da_append(&sb, '\n');
  }

  sb_appendf(&sb, "    .bss\n");
  da_foreach(Var *, p, &prog->vars) {
    Var *var = *p;
    if (var->init_value.kind == ARG_NONE) {
      sb_appendf(&sb, "    .globl "SV_Fmt"\n", SV_Arg(var->name));
      sb_appendf(&sb, "    .bss\n");
      sb_appendf(&sb, "    .align %ld\n", var->type.size);
      sb_appendf(&sb, "    .type "SV_Fmt", @object\n", SV_Arg(var->name));
      sb_appendf(&sb, "    .size "SV_Fmt", %ld\n", SV_Arg(var->name), var->type.size);
      sb_appendf(&sb, ""SV_Fmt":\n", SV_Arg(var->name));
      sb_appendf(&sb, "    .zero %ld\n", var->type.size);
    } else {
      sb_appendf(&sb, "    .globl "SV_Fmt"\n", SV_Arg(var->name));
      sb_appendf(&sb, "    .data\n");
      sb_appendf(&sb, "    .align %ld\n", var->type.size);
      sb_appendf(&sb, "    .type "SV_Fmt", @object\n", SV_Arg(var->name));
      sb_appendf(&sb, "    .size "SV_Fmt", %ld\n", SV_Arg(var->name), var->type.size);
      sb_appendf(&sb, ""SV_Fmt":\n", SV_Arg(var->name));
      switch (var->type.size) {
      case 1: sb_appendf(&sb, "    .byte ");  break;
      case 2: sb_appendf(&sb, "    .value "); break;
      case 4: sb_appendf(&sb, "    .long ");  break;
      case 8: sb_appendf(&sb, "    .quad ");  break;
      default: UNREACHABLE("");
      }
      dump_arg(&sb, &var->init_value);
      sb_appendf(&sb, "\n");
    }
  }

  sb_appendf(&sb, "    .text\n");
  for (size_t fn_i = 0; fn_i < prog->fn_list.count; ++fn_i) {
    Fn *fn = prog->fn_list.items[fn_i];

    //    String_View fn_name = sym_name(prog->global, fn);
    da_foreach(String_Builder, name, &fn->names) {
      String_View fn_name = sb_to_sv(*name);
      sb_appendf(&sb, "    .globl  "SV_Fmt"\n", SV_Arg(fn_name));
      sb_appendf(&sb, "    .type  "SV_Fmt", @function\n", SV_Arg(fn_name));
      sb_appendf(&sb, SV_Fmt":\n", SV_Arg(fn_name));
    }

    sb_appendf(&sb, "    pushq %%rbp\n");
    sb_appendf(&sb, "    movq  %%rsp, %%rbp\n");

    assert(fn->type.kind == TYPE_FN);
    X64RegMap map = x64_reg_alloc(fn->regs);
    sb_appendf(&sb, "    subq $%ld, %%rsp\n", map.memsize);
    for (size_t i = 0; i < fn->type.fn_type.arg_types.count; ++i) {
      X64_reg_to_vreg(&sb, param_regs[i], map.items[i]);
    }

    size_t rsp = map.memsize;
    for (size_t op_idx = 0; op_idx < fn->fn_body.count; ++op_idx) {
      Op *op = &fn->fn_body.items[op_idx];
      sb_appendf(&sb, "    // ");
      dump_op(&sb, op);
      static_assert(__op_kind_count == 11, "introduced more op kinds");
      switch(op->kind) {
      case OP_SET_REG:
        arg2reg(&sb, &map, &op->set_reg.arg, RAX);
        X64_reg_to_vreg(&sb, RAX, map.items[op->set_reg.reg.id]);
        break;
      case OP_LOAD:
        assert(op->load.src.type.size == 8);
        assert(op->load.dst.type.size <= 8);
        X64_vreg_to_reg(&sb, RAX, map.items[op->load.src.id]);
        x64_load(&sb, RBX, RAX, op->load.dst.type.size);
        X64_reg_to_vreg(&sb, RBX, map.items[op->load.dst.id]);
        break;
      case OP_STORE:
        assert(op->store.reg.type.size == 8);
        assert(op->store.arg.type.size <= 8);
        X64_vreg_to_reg(&sb, RAX, map.items[op->store.reg.id]);
        arg2reg(&sb, &map, &op->store.arg, RBX);
        x64_store(&sb, RAX, RBX, op->store.arg.type.size);
        break;
      case OP_ALLOCA: {
        assert(op->alloca.reg.type.size == 8);
        sb_appendf(&sb, "    leaq -%ld(%%rbp), %%RAX\n", rsp);
        size_t new_rsp = (rsp + op->alloca.memsize - 1) / 16 * 16 + 16;
        sb_appendf(&sb, "    subq $%ld, %%rsp\n", new_rsp - rsp);
        rsp = new_rsp;
        X64_reg_to_vreg(&sb, RAX, map.items[op->alloca.reg.id]);
      } break;
      case OP_DEALLOC:
        TODO("");
        break;
      case OP_INVOKE: {
        for (int i = op->invoke.args.count - 1; i >= 0; --i) {
          if ((size_t)i > PARAM_REGS_CNT) {
            arg2reg(&sb, &map, &op->invoke.args.items[i], RAX);
            sb_appendf(&sb, "    pushq %%rax\n");
          } else {
            arg2reg(&sb, &map, &op->invoke.args.items[i], param_regs[i]);
          }
        }

        switch (op->invoke.fn.kind) {
        case ARG_REG:
          arg2reg(&sb, &map, &op->invoke.fn, RAX);
          sb_appendf(&sb, "    call *%%rax\n");
          break;
        case ARG_FN: {
          assert(op->invoke.fn.fn->names.count != 0);
          String_View name = sb_to_sv(da_first(&op->invoke.fn.fn->names));
          sb_appendf(&sb, "    call "SV_Fmt"@PLT\n",
                     SV_Arg(name));
        } break;
        case ARG_EXT: {
          sb_appendf(&sb, "    call "SV_Fmt"@PLT\n",
                     SV_Arg(op->invoke.fn.ext));
        } break;
        case ARG_NONE:
        case ARG_LIT_INT:
        case ARG_LIT_STR:
          UNREACHABLE("this argument is not callable");
        default:
          UNREACHABLE("unkown argument");
        }

        if (!op->invoke.ret_ignore) {
          X64_reg_to_vreg(&sb, RAX, map.items[op->invoke.ret.id]);
        }
      } break;
      case OP_RETURN:
        if (op->ret_val.kind != ARG_NONE) {
          arg2reg(&sb, &map, &op->ret_val, RAX);
        }
        sb_appendf(&sb, "    leave\n");
        sb_appendf(&sb, "    ret\n");
        break;
      case OP_BINOP: {
        TypeKind type_kind = op->binop.lhs.type.kind;
        size_t   size      = op->binop.lhs.type.size > op->binop.rhs.type.size?
          op->binop.lhs.type.size : op->binop.rhs.type.size;
        char s = cmd_suff[size];

        if (op->binop.lhs.type.size < size) {
          sb_appendf(&sb, "    xor%c %s, %s\n",
                     s, regs[RAX][size], regs[RAX][size]);
        }
        arg2reg(&sb, &map, &op->binop.lhs, RAX);

        if (op->binop.rhs.type.size < size) {
          sb_appendf(&sb, "    xor%c %s, %s\n",
                     s, regs[RBX][size], regs[RBX][size]);
        }
        arg2reg(&sb, &map, &op->binop.rhs, RBX);

        static_assert(__binop_kind_count == 11, "introduced more binop kinds");
        switch (op->binop.kind) {
        case BINOP_ADD:
          sb_appendf(&sb, "    add%c %s, %s\n",
                     s, regs[RBX][size], regs[RAX][size]);
          break;
        case BINOP_SUB:
          sb_appendf(&sb, "    sub%c %s, %s\n",
                     s, regs[RBX][size], regs[RAX][size]);
          break;
        case BINOP_MUL:
          sb_appendf(&sb, "    mul%c %s\n",
                     s, regs[RBX][size]);
          break;
        case BINOP_DIV:
          sb_appendf(&sb, "    div%c %s\n",
                     s, regs[RBX][size]);
          break;
        case BINOP_MOD:
          sb_appendf(&sb, "    div%c %s\n",
                     s, regs[RBX][size]);
          sb_appendf(&sb, "    mov%c %s, %s\n",
                     s, regs[RDX][size], regs[RAX][size]);
          break;
        case BINOP_EQ:
          sb_appendf(&sb, "    cmp %s, %s\n",
                     regs[RBX][size], regs[RAX][size]);
          sb_appendf(&sb, "    sete %%al\n");
          break;
        case BINOP_NEQ:
          sb_appendf(&sb, "    cmp %s, %s\n",
                     regs[RBX][size], regs[RAX][size]);
          sb_appendf(&sb, "    setne %%al\n");
          break;
        case BINOP_LS:
          sb_appendf(&sb, "    cmp %s, %s\n",
                     regs[RBX][size], regs[RAX][size]);
          if (type_kind == TYPE_INT) {
            sb_appendf(&sb, "    setl %%al\n");
          } else if (type_kind == TYPE_UINT){
            sb_appendf(&sb, "    setb %%al\n");
          } else {
            UNREACHABLE("unexpected type");
          }
          break;
        case BINOP_GT:
          sb_appendf(&sb, "    cmp %s, %s\n",
                     regs[RBX][size], regs[RAX][size]);
          if (type_kind == TYPE_INT) {
            sb_appendf(&sb, "    setg %%al\n");
          } else if (type_kind == TYPE_UINT){
            sb_appendf(&sb, "    seta %%al\n");
          } else {
            UNREACHABLE("unexpected type");
          }
          break;
        case BINOP_LE:
          sb_appendf(&sb, "    cmp %s, %s\n",
                     regs[RBX][size], regs[RAX][size]);
          if (type_kind == TYPE_INT) {
            sb_appendf(&sb, "    setle %%al\n");
          } else if (type_kind == TYPE_UINT){
            sb_appendf(&sb, "    setbe %%al\n");
          } else {
            UNREACHABLE("unexpected type");
          }
          break;
        case BINOP_GE:
          sb_appendf(&sb, "    cmp %s, %s\n",
                     regs[RBX][size], regs[RAX][size]);
          if (type_kind == TYPE_INT) {
            sb_appendf(&sb, "    setge %%al\n");
          } else if (type_kind == TYPE_UINT){
            sb_appendf(&sb, "    setae %%al\n");
          } else {
            UNREACHABLE("unexpected type");
          }
          break;
        default: UNREACHABLE("");
        }

        X64_reg_to_vreg(&sb, RAX, map.items[op->binop.dst.id]);
      }  break;
      case OP_JMP:
        sb_appendf(&sb, "    jmp .fn_%ld.label_%ld\n", fn_i, op->jmp.label);
        break;
      case OP_JMP_ELSE: {
        size_t size = arg2reg(&sb, &map, &op->jmp.cond, RAX);
        sb_appendf(&sb, "    cmp $0, %s\n", regs[RAX][size]);
        sb_appendf(&sb, "    je .fn_%ld.label_%ld\n", fn_i, op->jmp.label);
      } break;
      case OP_LABEL:
        sb_appendf(&sb, ".fn_%ld.label_%ld:\n", fn_i, op_idx);
        break;
      default:
        UNREACHABLE("op");
      }
    }
  }
  return sb;
}

bool build_x86_64_native(const char *filename, const Program *prog)
{
  bool result;
  size_t mark = temp_save();

  String_Builder code = gen_code_x86_64_gas(prog);
  char *asm_file = temp_sprintf("%s.s", filename);
  if (!write_entire_file(asm_file, code.items, code.count)) return_defer(false);

  Cmd cmd = {0};
  cmd_append(&cmd, "cc", "-o", filename, asm_file);
  if(!cmd_run(&cmd)) return_defer(false);

  return_defer(true);

 defer:
  temp_rewind(mark);
  da_free(code);
  if (cmd.capacity > 0) cmd_free(cmd);
  return result;
}

typedef enum {
  TARGET_IR,
  TARGET_X86_64_NATIVE,
} Target;

static const struct {
  const char *name;
  const Target target;
} TARGETS[] = {
  { "ir",        TARGET_IR        },
  { "x86_64-native", TARGET_X86_64_NATIVE },
};

static struct {
  const char *program;
  Target target;
  File_Paths files;
  const char *outfile;
  bool only_lexer;
  bool run;
} mcc_args;

void usage(FILE *stream)
{
  fprintf(stream, "Usage: %s [OPTIONS] file...\n", mcc_args.program);
  fprintf(stream, "OPTIONS:\n");
}

bool parse_mcc_args(int argc, char **argv)
{
  mcc_args.program = argv[0];
  mcc_args.target  = TARGET_X86_64_NATIVE;
  mcc_args.outfile = NULL;

  bool result = true;

  int i = 1;
  while (i < argc) {
    if (*argv[i] != '-') {
      da_append(&mcc_args.files, argv[i++]);
    } else if (strcmp(argv[i], "-o") == 0) {
      i += 1;
      if (i >= argc || *argv[i] == '-') {
        fprintf(stderr, "%s:error: missing filename after '-o'\n", mcc_args.program);
        result = false;
      } else {
        mcc_args.outfile = argv[i++];
      }
    } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--target") == 0) {
      i += 1;
      if (i >= argc || *argv[i] == '-') {
        fprintf(stderr, "%s:error: missing a target after '%s'", mcc_args.program, argv[i - 1]);
        result = false;
      }

      size_t j = 0;
      for (; j < ARRAY_LEN(TARGETS); ++j) {
        if (strcmp(TARGETS[j].name, argv[i]) == 0) {
          mcc_args.target = TARGETS[j].target;
          i += 1;
          break;
        }
      }
      if (j >= ARRAY_LEN(TARGETS)) {
        fprintf(stderr, "%s:error: Unsupported target '%s'\n", mcc_args.program, argv[i]);
        result = false;
      }
    } else if (strcmp(argv[i], "-l") == 0) {
      mcc_args.only_lexer = true;
      i += 1;
    } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--run")) {
      mcc_args.run = true;
      i += 1;
    }
  }

  if (mcc_args.files.count == 0) {
    fprintf(stderr, "%s:error: no input files\n", mcc_args.program);
    return false;
  }

  if (mcc_args.outfile == NULL) {
    char *outfile = temp_file_name(mcc_args.files.items[0]);
    char *ext = strrchr(outfile, '.');
    if (ext != NULL) {
      *ext = '\0';
    }
    mcc_args.outfile = outfile;
  }

  return result;
}

bool dump_all_tokens(Lexer *l)
{
  while (lexer_next(l)) {
    Token t = l->current;
    printf(CS_Fmt "%s: "SV_Fmt"\n",
           CS_Arg(t.start),
           token_name(t.kind),
           SV_Arg(t.str));
  }

  Token t = l->current;
  printf(CS_Fmt "%s: "SV_Fmt"\n",
         CS_Arg(t.start),
         token_name(t.kind),
         SV_Arg(t.str));
  if (t.kind == TOKEN_ERR) return false;

  return true;
}

int main(int argc, char **argv)
{
  if (!parse_mcc_args(argc, argv)) {
    exit(1);
  }

  int result = 0;

  if (mcc_args.files.count > 1) {
    TODO("support multiple files");
  }

  Lexer lexer = {0};
  if (!lexer_init(&lexer, sv_from_cstr(mcc_args.files.items[0])))
    return_defer(1);

  if (mcc_args.only_lexer) {
    return_defer(dump_all_tokens(&lexer)? 0 : 1);
  }

  Program *prog = compile_program(&lexer);
  if (prog == NULL) {
    fprintf(stderr,
            "fatal error: failed to compile file %s\n",
            mcc_args.files.items[0]);
    return_defer(1);
  }

  switch(mcc_args.target) {
  case TARGET_IR:
    if(!build_ir(mcc_args.outfile, prog)) return_defer(1);
    break;
  case TARGET_X86_64_NATIVE:
    if (!build_x86_64_native(mcc_args.outfile, prog)) return_defer(1);
    break;
  default: UNREACHABLE("target");
  }

  if (mcc_args.run) {
    Cmd cmd = {0};
    cmd_append(&cmd, temp_sprintf("./%s", mcc_args.outfile));
    int result = cmd_run(&cmd);
    da_free(cmd);
    return_defer(result);
  }

  return_defer(0);

 defer:
  arena_free(get_arena());
  if (result) fprintf(stderr, "compilation terminated\n");

  return result;
}

#define MCC_IR_IMPLEMENTATION
#include "ir.h"

#define MCC_LEXER_IMPLEMENTATION
#include "lexer.h"

#define MCC_TYPE_IMPLEMENTATION
#include "type.h"

#define MCC_AST_IMPLEMENTATION
#include "ast.h"

#define MCC_3RD_WRAPPER_IMPLEMENTATION
#include "3rd_wrapper.h"
