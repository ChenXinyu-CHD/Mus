#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "parser.h"

#include "stb_c_lexer.h"

// nob.h uses this macro
// but debian hurd do not have it
// TODO: find out the real limit on PATH_MAX, and feed back to upstream
#ifndef PATH_MAX
#define PATH_MAX 2048
#endif

#define NOB_IMPLEMENTATION
#include "nob.h"

#define LEXER_BUFFER_SIZE 1<<20
static char lexer_buffer[LEXER_BUFFER_SIZE];

void append_str_lit(String_Builder *sb, char *str)
{
  da_append(sb, '"');
  for (char *it = str; *it != '\0'; ++it) {
    switch(*it) {
    case '\n': sb_appendf(sb, "\\n");  break;
    case '\b': sb_appendf(sb, "\\b");  break;
    case '\t': sb_appendf(sb, "\\t");  break;
    case '\r': sb_appendf(sb, "\\r");  break;
    case '\v': sb_appendf(sb, "\\v");  break;
    case '\'': sb_appendf(sb, "'");    break;
    case '\"': sb_appendf(sb, "\\\""); break;
    case '\a': sb_appendf(sb, "\\a");  break;
    default: da_append(sb, *it);
    }
  }
  
  da_append(sb, '"');
}

void gen_arg_ir(String_Builder *sb, Arg arg)
{
  switch(arg.kind) {
  case ARG_NAME:
    sb_appendf(sb, "%s", arg.name);
    break;
  case ARG_VAR_LOC:
    sb_appendf(sb, "%%local[%ld]", arg.label);
    break;
  case ARG_LIT_INT:
    sb_appendf(sb, "%d", arg.num_int);
    break;
  case ARG_LIT_STR:
    sb_appendf(sb, ".str_lits[%ld]", arg.label);
    break;
  default: UNREACHABLE("arg");
  }
}

String_Builder gen_code_ir(const Program *prog)
{
  String_Builder sb = {0};

  sb_appendf(&sb, "extern:");
  da_foreach (Extern, ext, &prog->externs) {
    sb_appendf(&sb, " %s", ext->name);
  }
  sb_appendf(&sb, "\n\n");
  
  sb_appendf(&sb, ".str_lits:\n");
  for (size_t i = 0; i < prog->str_lits.count; ++i) {
    sb_appendf(&sb, "    .%ld: ", i);
    append_str_lit(&sb, prog->str_lits.items[i]);
    sb_append(&sb, '\n');
  }

  sb_appendf(&sb, "\n");
  
  da_foreach (Fn, fn, &prog->fn_list) {
    sb_appendf(&sb, "%s:\n", fn->name);
    da_foreach (Op, op, &fn->fn_body) {
      switch(op->kind) {
      case OP_INVOKE:
        sb_appendf(&sb, "    call ");
        gen_arg_ir(&sb, op->fn);
        da_foreach (Arg, arg, &op->args) {
          sb_appendf(&sb, ", ");
          gen_arg_ir(&sb, *arg);
        }
        sb_append(&sb, '\n');
        break;
      case OP_RETURN:
        sb_appendf(&sb, "    ret ");
        if (op->ret_val.kind != ARG_NONE) {
          gen_arg_ir(&sb, op->ret_val);
        }
        sb_append(&sb, '\n');
        break;
      case OP_SET_VAR:
        sb_appendf(&sb, "    set ");
        gen_arg_ir(&sb, op->var);
        sb_appendf(&sb, " = ");
        gen_arg_ir(&sb, op->val);
        sb_append(&sb, '\n');
        break;
      default:
        UNREACHABLE("op");
      }
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

size_t ceil16(size_t x) {
  if (x % 16 > 0) {
    x += 16 - (x % 16);
  }
  return x;
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
  da_foreach (Fn, fn, &prog->fn_list) {
    sb_appendf(&sb, "    .globl  %s\n", fn->name);
    sb_appendf(&sb, "    .type  %s, @function\n", fn->name);
    sb_appendf(&sb, "%s:\n", fn->name);
    sb_appendf(&sb, "    pushq %%rbp\n");
    sb_appendf(&sb, "    movq  %%rsp, %%rbp\n");
    if (fn->local.count != 0) {
      sb_appendf(&sb, "    subq $%ld, %%rsp\n", ceil16(fn->local.count * 8));
    }
      
    da_foreach (Op, op, &fn->fn_body) {
      switch(op->kind) {
      case OP_INVOKE:
        for (int i = op->args.count - 1; i >= 0; --i) {
          static char *reg[] = {
            "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
          };
          switch(op->args.items[i].kind) {
          case ARG_LIT_INT:
            if ((size_t)i > ARRAY_LEN(reg)) {
              sb_appendf(&sb, "    pushq $%d\n", op->args.items[i].num_int);
            } else {
              sb_appendf(&sb, "    movq $%d, %s\n", op->args.items[i].num_int, reg[i]);
            }
            break;
          case ARG_LIT_STR:
            if ((size_t)i > ARRAY_LEN(reg)) {
              sb_appendf(&sb, "    leaq .S_%ld(%%rip), %%rax\n", op->args.items[i].label);
              sb_appendf(&sb, "    pushq %%rax\n");
            } else {
              sb_appendf(&sb, "    leaq .S_%ld(%%rip), %s\n", op->args.items[i].label, reg[i]);
            }
            break;
          case ARG_VAR_LOC:
            if ((size_t)i > ARRAY_LEN(reg)) {
              sb_appendf(&sb, "    movq -%ld(%%rbp), %%rax\n", op->args.items[i].label * 8);
              sb_appendf(&sb, "    pushq %%rax\n");
            } else {
              sb_appendf(&sb, "    movq -%ld(%%rbp), %s\n", op->args.items[i].label * 8, reg[i]);
            }
            break;
          default: UNREACHABLE("arg");
          }
        }
          
        switch(op->fn.kind) {
        case ARG_NAME:
          sb_appendf(&sb, "    call %s\n", op->fn.name);
          break;
        default: UNREACHABLE("arg");
        }

        break;
      case OP_RETURN:
        switch(op->ret_val.kind) {
        case ARG_NONE: break;
        case ARG_LIT_INT:
          sb_appendf(&sb, "    movq $%d, %%rax\n", op->ret_val.num_int);
          break;
        case ARG_VAR_LOC:
          sb_appendf(&sb, "    movq -%ld(%%rbp), %%rax\n", op->ret_val.label * 8);
          break;
        default: UNREACHABLE("arg");
        }
        
        sb_appendf(&sb, "    leave\n");
        sb_appendf(&sb, "    ret\n");
        break;
      case OP_SET_VAR: {
        assert(op->var.kind == ARG_VAR_LOC);

        size_t label = op->var.label;
        switch(op->val.kind) {
        case ARG_LIT_INT:
          sb_appendf(&sb, "    movq $%d, -%ld(%%rbp)\n", op->val.num_int, label * 8);
          break;
        case ARG_VAR_LOC:
          sb_appendf(&sb, "    movq -%ld(%%rbp), %%rax\n", op->val.label * 8);
          sb_appendf(&sb, "    movq %%rax, -%ld(%%rbp)\n", label * 8);
          break;
        default: UNREACHABLE("arg");
        }
        break;
      } default:
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
  nob_cmd_append(&cmd, "cc", "-o", filename, asm_file);
  if(!cmd_run(&cmd)) return_defer(false);
  
  if (!write_entire_file(asm_file, code.items, code.count)) return_defer(false);

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
  
  if (!read_entire_file(mcc_args.files.items[0], &sb)) {
    fprintf(stderr, "fatal error: can not read file %s\n", mcc_args.files.items[0]);
    return_defer(1);
  }

  stb_lexer lex;
  stb_c_lexer_init(&lex, sb.items, sb.items + sb.count, lexer_buffer, LEXER_BUFFER_SIZE);

  if (!compile_file(&lex, mcc_args.files.items[0], &prog)) {
    fprintf(stderr, "fatal error: failed to compile file %s\n", argv[1]);
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
  
  return_defer(0);

 defer:
  if (sb.capacity > 0) da_free(sb);
  destroy_program(&prog);
  if (result) fprintf(stderr, "compilation terminated\n");
  
  return result;
}
