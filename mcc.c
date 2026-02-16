#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "parser.h"

#include "stb_c_lexer.h"

#define NOB_IMPLEMENTATION
#include "nob.h"

#define LEXER_BUFFER_SIZE 1<<20
static char lexer_buffer[LEXER_BUFFER_SIZE];

void gen_arg_ir(String_Builder *sb, Arg arg)
{
  switch(arg.kind) {
  case ARG_NAME:
    sb_appendf(sb, arg.name);
    break;
  case ARG_LIT_INT:
    sb_appendf(sb, "%d", arg.num_int);
    break;
  default: UNREACHABLE("arg");
  }
}

String_Builder gen_code_ir(const SymbolTable *syms)
{
  String_Builder sb = {0};

  sb_appendf(&sb, "extern:");
  da_foreach (Symbol, sym, syms) {
    if (sym->external) {
      sb_appendf(&sb, " %s", sym->name);
    }
  }

  sb_appendf(&sb, "\n\n");
  
  da_foreach (Symbol, sym, syms) {
    if (!sym->external) {
      sb_appendf(&sb, "%s:\n", sym->name);
      da_foreach (Op, op, &sym->fn_body) {
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
          gen_arg_ir(&sb, op->ret_val);
          sb_append(&sb, '\n');
          break;
        default:
          UNREACHABLE("op");
        }
      }
    }
  }

  return sb;
}

String_Builder gen_code_x64_linux(const SymbolTable *syms)
{
  UNUSED(syms);
  TODO("gen_code_x64_linux");
}

typedef enum {
  TARGET_IR,
  TARGET_X64_LINUX,
} Target;

static const struct {
  const char *name;
  const Target target;
} TARGETS[] = {
  { "ir",        TARGET_IR        },
  { "x64_linux", TARGET_X64_LINUX },
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

bool parse_args(int argc, char **argv)
{
  mcc_args.program = argv[0];
  mcc_args.target  = TARGET_IR;
  mcc_args.outfile = "a.out";

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
    result = false;
  }

  return result;
}

int main(int argc, char **argv)
{
  if (!parse_args(argc, argv)) {
    exit(1);
  }  
  
  int result        = 0;
  String_Builder sb = {0};
  SymbolTable syms  = {0};
  String_Builder code;

  if (mcc_args.files.count > 1) {
    TODO("support multiple files");
  }
  
  if (!read_entire_file(mcc_args.files.items[0], &sb)) {
    fprintf(stderr, "fatal error: can not read file %s\n", mcc_args.files.items[0]);
    return_defer(1);
  }

  stb_lexer lex;
  stb_c_lexer_init(&lex, sb.items, sb.items + sb.count, lexer_buffer, LEXER_BUFFER_SIZE);

  if (!compile_file(&lex, argv[1], &syms)) {
    fprintf(stderr, "fatal error: failed to compile file %s\n", argv[1]);
    return_defer(1);
  }

  switch(mcc_args.target) {
  case TARGET_IR:
    code = gen_code_ir(&syms);
    break;
  case TARGET_X64_LINUX:
    code = gen_code_x64_linux(&syms);
    break;
  default: UNREACHABLE("target");
  }
  
  write_entire_file(mcc_args.outfile, code.items, code.count);

  return_defer(0);

 defer:
  if (sb.capacity > 0)   da_free(sb);
  if (syms.capacity > 0) destroy_symtable(&syms);
  if (code.capacity > 0) da_free(code);
  if (result) fprintf(stderr, "compilation terminated\n");
  
  return result;
}
