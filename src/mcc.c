#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "lexer.h"
#include "parser.h"
#include "utils.h"

void append_str_lit(String_Builder *sb, String_View str)
{
  sb_appendf(sb, "\""SV_Fmt"\"", SV_Arg(str));
}

String_Builder gen_code_ir(const Program *prog)
{
  String_Builder sb = {0};

  sb_appendf(&sb, "extern:");
  da_foreach (Extern*, ext, &prog->externs) {
    sb_appendf(&sb, " "SV_Fmt"", SV_Arg((*ext)->linkname));
  }
  sb_appendf(&sb, "\n\n");

  for (size_t i = 0; i < prog->str_lits.count; ++i) {
    sb_appendf(&sb, ".S_%ld = ", i);
    append_str_lit(&sb, prog->str_lits.items[i]);
    sb_append(&sb, '\n');
  }

  sb_appendf(&sb, "\n");

  for (size_t i = 0; i < prog->fn_list.count; ++i) {
    Fn* fn = prog->fn_list.items[i];
    String_View fn_name = sb_to_sv(fn->name);
    //    String_View fn_name = sym_name(prog->global, fn);
    if (fn_name.count != 0) {
      sb_appendf(&sb, SV_Fmt":\n", SV_Arg(fn_name));
    }

    sb_appendf(&sb, ".fn_%ld:\n", i);

    da_foreach (Op, op, &fn->fn_body) {
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

static char *param_regs[] = {
  "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
};

#define PARAM_REGS_CNT ARRAY_LEN(param_regs)

static void build_var_offset_x86_64_gas(VarList *vars, size_t arg_count)
{
  if (arg_count > PARAM_REGS_CNT)
    TODO("support more than PARAM_REGS_CNT args");
  size_t reg_args = arg_count < PARAM_REGS_CNT? arg_count: PARAM_REGS_CNT;

  vars->memsize = 0;
  for (size_t i = 0; i < reg_args; ++i) {
    Var *arg = vars->items[i];
    size_t size = arg->type.size;
    assert(size != 0);
    vars->memsize = ceil_to(vars->memsize, size) + size;
    arg->offset = -vars->memsize;
  }

  for (size_t i = reg_args; i < vars->count; ++i) {
    Var *var = vars->items[i];
    size_t size = var->type.size;
    assert(size != 0);
    vars->memsize = ceil_to(vars->memsize, size) + size;
    var->offset = -vars->memsize;
  }
  vars->memsize = ceil_to(vars->memsize, 16);
}

static void rax2rbp_offset(String_Builder *sb, size_t size, ptrdiff_t offset)
{
  switch (size) {
  case 1:
    sb_appendf(sb, "    movb %%al, %ld(%%rbp)\n", offset);
    break;
  case 2:
    sb_appendf(sb, "    movw %%ax, %ld(%%rbp)\n", offset);
    break;
  case 4:
    sb_appendf(sb, "    movl %%eax, %ld(%%rbp)\n", offset);
    break;
  case 8:
    sb_appendf(sb, "    movq %%rax, %ld(%%rbp)\n", offset);
    break;
  default:
    UNREACHABLE("");
  }
}

static void rbp_offset2rax(String_Builder *sb, size_t size, ptrdiff_t offset)
{
  if (size != 8) {
    sb_appendf(sb, "    xor %%rax, %%rax\n");
  }
  switch (size) {
  case 1:
    sb_appendf(sb, "    movb %ld(%%rbp), %%al\n", offset);
    break;
  case 2:
    sb_appendf(sb, "    movw %ld(%%rbp), %%ax\n", offset);
    break;
  case 4:
    sb_appendf(sb, "    movl %ld(%%rbp), %%eax\n", offset);
    break;
  case 8:
    sb_appendf(sb, "    movq %ld(%%rbp), %%rax\n", offset);
    break;
  default:
    UNREACHABLE("");
  }
}

static void arg2rax(String_Builder *sb, Arg *arg)
{
  static_assert(__arg_kind_count == 6, "introduced more arg kinds");
  switch (arg->kind) {
  case ARG_NONE:
    sb_appendf(sb, "    mov %%rax, %%rax\n");
    break;
  case ARG_VAR: {
    Var *var = arg->var;
    rbp_offset2rax(sb, var->type.size, var->offset);
  } break;
  case ARG_FN: {
    String_View name = sb_to_sv(arg->fn->name);
    sb_appendf(sb, "    leaq "SV_Fmt"@PLT(%%rip), %%rax\n",
               SV_Arg(name));
  } break;
  case ARG_EXTERN:
    sb_appendf(sb, "    leaq "SV_Fmt"@PLT(%%rip), %%rax\n",
               SV_Arg(arg->ext->linkname));
  break;
  case ARG_LIT_INT:
    sb_appendf(sb, "    movq $%d, %%rax\n", arg->num_int);
    break;
  case ARG_LIT_STR:
    sb_appendf(sb, "    leaq .S_%ld(%%rip), %%rax\n", arg->str_label);
    break;
  default:
    UNREACHABLE("");
  }
}

String_Builder gen_code_x86_64_gas(const Program *prog)
{
  String_Builder sb = {0};

  sb_appendf(&sb, "    .text\n");
  sb_appendf(&sb, "    .section .rodata\n");
  for (size_t i = 0; i < prog->str_lits.count; ++i) {
    sb_appendf(&sb, ".S_%ld:\n", i);
    sb_appendf(&sb, "    .string ");
    append_str_lit(&sb, prog->str_lits.items[i]);
    da_append(&sb, '\n');
  }

  sb_appendf(&sb, "    .text\n");
  for (size_t fn_i = 0; fn_i < prog->fn_list.count; ++fn_i) {
    Fn *fn = prog->fn_list.items[fn_i];

    //    String_View fn_name = sym_name(prog->global, fn);
    String_View fn_name = sb_to_sv(fn->name);
    sb_appendf(&sb, "    .globl  "SV_Fmt"\n", SV_Arg(fn_name));
    sb_appendf(&sb, "    .type  "SV_Fmt", @function\n", SV_Arg(fn_name));
    sb_appendf(&sb, SV_Fmt":\n", SV_Arg(fn_name));

    sb_appendf(&sb, "    pushq %%rbp\n");
    sb_appendf(&sb, "    movq  %%rsp, %%rbp\n");

    assert(fn->type.kind == TYPE_FN);
    build_var_offset_x86_64_gas(&fn->vars, fn->type.fn_type.arg_types.count);
    sb_appendf(&sb, "    subq $%ld, %%rsp\n", fn->vars.memsize);

    for (size_t i = 0; i < PARAM_REGS_CNT; ++i) {
      if (i >= fn->type.fn_type.arg_types.count) break;
      sb_appendf(&sb, "    movq %s, %%rax\n", param_regs[i]);
      Var *arg = fn->vars.items[i];
      rax2rbp_offset(&sb, arg->type.size, arg->offset);
    }

    for (size_t op_idx = 0; op_idx < fn->fn_body.count; ++op_idx) {
      Op *op = &fn->fn_body.items[op_idx];
      sb_appendf(&sb, "    // ");
      dump_op(&sb, op);
      static_assert(__op_kind_count == 7, "introduced more op kinds");
      switch(op->kind) {
      case OP_INVOKE: {
        for (int i = op->invoke.args.count - 1; i >= 0; --i) {
          arg2rax(&sb, &op->invoke.args.items[i]);
          if ((size_t)i > PARAM_REGS_CNT) {
            sb_appendf(&sb, "    pushq %%rax\n");
          } else {
            sb_appendf(&sb, "    movq %%rax, %s\n", param_regs[i]);
          }
        }
        arg2rax(&sb, &op->invoke.fn);
        sb_appendf(&sb, "    call *%%rax\n");

        if (!op->invoke.ret_ignore) {
          assert(op->invoke.ret.kind == ARG_VAR);
          Var *ret = op->invoke.ret.var;
          rax2rbp_offset(&sb, ret->type.size, ret->offset);
        }
      } break;
      case OP_RETURN:
        arg2rax(&sb, &op->ret_val);
        sb_appendf(&sb, "    leave\n");
        sb_appendf(&sb, "    ret\n");
        break;
      case OP_BINOP: {
        arg2rax(&sb, &op->binop.rhs);
        sb_appendf(&sb, "    movq %%rax, %%rbx\n");
        arg2rax(&sb, &op->binop.lhs);

        static_assert(__binop_kind_count == 11, "introduced more binop kinds");
        switch (op->binop.kind) {
        case BINOP_ADD:
          sb_appendf(&sb, "    addq %%rbx, %%rax\n");
          break;
        case BINOP_SUB:
          sb_appendf(&sb, "    subq %%rbx, %%rax\n");
          break;
        case BINOP_MUL:
          sb_appendf(&sb, "    mulq %%rbx\n");
          break;
        case BINOP_DIV:
          sb_appendf(&sb, "    divq %%rbx\n");
          break;
        case BINOP_MOD:
          sb_appendf(&sb, "    divq %%rbx\n");
          sb_appendf(&sb, "    movq %%rdx, %%rax\n");
          break;
        case BINOP_EQ:
          sb_appendf(&sb, "    cmp %%rbx, %%rax\n");
          sb_appendf(&sb, "    movq $0, %%rax\n");
          sb_appendf(&sb, "    movq $1, %%rbx\n");
          sb_appendf(&sb, "    cmoveq %%rbx, %%rax\n");
          break;
        case BINOP_NEQ:
          sb_appendf(&sb, "    cmp %%rbx, %%rax\n");
          sb_appendf(&sb, "    movq $0, %%rax\n");
          sb_appendf(&sb, "    movq $1, %%rbx\n");
          sb_appendf(&sb, "    cmovneq %%rbx, %%rax\n");
          break;
          // TODO: add support for unsigned integers
        case BINOP_LS:
          sb_appendf(&sb, "    cmp %%rbx, %%rax\n");
          sb_appendf(&sb, "    movq $0, %%rax\n");
          sb_appendf(&sb, "    movq $1, %%rbx\n");
          sb_appendf(&sb, "    cmovlq %%rbx, %%rax\n");
          break;
        case BINOP_GT:
          sb_appendf(&sb, "    cmp %%rbx, %%rax\n");
          sb_appendf(&sb, "    movq $0, %%rax\n");
          sb_appendf(&sb, "    movq $1, %%rbx\n");
          sb_appendf(&sb, "    cmovgq %%rbx, %%rax\n");
          break;
        case BINOP_LE:
          sb_appendf(&sb, "    cmp %%rbx, %%rax\n");
          sb_appendf(&sb, "    movq $0, %%rax\n");
          sb_appendf(&sb, "    movq $1, %%rbx\n");
          sb_appendf(&sb, "    cmovleq %%rbx, %%rax\n");
          break;
        case BINOP_GE:
          sb_appendf(&sb, "    cmp %%rbx, %%rax\n");
          sb_appendf(&sb, "    movq $0, %%rax\n");
          sb_appendf(&sb, "    movq $1, %%rbx\n");
          sb_appendf(&sb, "    cmovgeq %%rbx, %%rax\n");
          break;
        default: UNREACHABLE("");
        }

        assert(op->binop.dst.kind == ARG_VAR);
        Var *var = op->binop.dst.var;
        rax2rbp_offset(&sb, var->type.size, var->offset);
      }  break;
      case OP_SET_VAR: {
        arg2rax(&sb, &op->set_var.val);
        assert(op->set_var.var.kind == ARG_VAR);
        Var *var = op->set_var.var.var;
        rax2rbp_offset(&sb, var->type.size, var->offset);
      } break;
      case OP_JMP:
        sb_appendf(&sb, "    jmp .fn_%ld.label_%ld\n", fn_i, op->jmp.label);
        break;
      case OP_JMP_ELSE:
        arg2rax(&sb, &op->jmp.cond);
        sb_appendf(&sb, "    cmp $0, %%rax\n");
        sb_appendf(&sb, "    je .fn_%ld.label_%ld\n", fn_i, op->jmp.label);
        break;
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

  int result        = 0;
  String_Builder sb = {0};
  Program prog      = {0};

  if (mcc_args.files.count > 1) {
    TODO("support multiple files");
  }

  Lexer lexer = {0};
  if (!lexer_init(&lexer, sv_from_cstr(mcc_args.files.items[0])))
    return_defer(1);

  if (mcc_args.only_lexer) {
    return_defer(dump_all_tokens(&lexer)? 0 : 1);
  }

  if (!compile_program(&lexer, &prog)) {
    fprintf(stderr,
            "fatal error: failed to compile file %s\n",
            mcc_args.files.items[0]);
    return_defer(1);
  }

  switch(mcc_args.target) {
  case TARGET_IR:
    if(!build_ir(mcc_args.outfile, &prog)) return_defer(1);
    break;
  case TARGET_X86_64_NATIVE:
    if (!build_x86_64_native(mcc_args.outfile, &prog)) return_defer(1);
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
  if (sb.capacity > 0) da_free(sb);
  destroy_program(&prog);
  if (result) fprintf(stderr, "compilation terminated\n");

  return result;
}

#define MCC_UTILS_IMPLEMENTATION
#include "utils.h"

#define MCC_LEXER_IMPLEMENTATION
#include "lexer.h"

#define NOB_IMPLEMENTATION
#include "nob.h"

#define MCC_TYPE_IMPLEMENTATION
#include "type.h"

#define MCC_AST_IMPLEMENTATION
#include "ast.h"

#define HT_IMPLEMENTATION
#include "ht.h"

