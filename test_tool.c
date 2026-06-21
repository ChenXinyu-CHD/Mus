#define NOB_IMPLEMENTATION
#include "3rd/nob.h"

#define WORK_DIR "./build/"
#define SRC_DIR  "../example/"
#define TEST_DIR "../tests/"

int main(int argc, char **argv)
{
  shift(argv, argc);
  char *action   = shift(argv, argc);
  char *filename = shift(argv, argc);

  assert(argc == 0);

  if (!set_current_dir(WORK_DIR)) {
    nob_log(NOB_ERROR, "failed to cd "WORK_DIR);
    return 1;
  }

  char *code   = temp_sprintf(SRC_DIR"%s",      filename);
  char *expect = temp_sprintf(TEST_DIR"%s.out", filename);
  char *out    = temp_sprintf("./%s.out",       filename);

  if (strcmp(action, "test") == 0) {
    Cmd cmd = {0};
    cmd_append(&cmd, "../mcc", "-r", code);
    cmd_run(&cmd, .stdout_path = out, .stderr_path = out);

    String_Builder cmd_out = {0};
    if (!read_entire_file(out, &cmd_out)) return 1;
    String_Builder cmd_expect = {0};
    if (!read_entire_file(expect, &cmd_expect)) return 1;

    if (sv_eq(sb_to_sv(cmd_out), sb_to_sv(cmd_expect))) {
      nob_log(NOB_INFO, "%s...ok\n", filename);
      return 0;
    } else {
      nob_log(NOB_ERROR, "%s...err\n", filename);
      return 1;
    }
  } else if (strcmp(action, "update") == 0) {
    Cmd cmd = {0};
    cmd_append(&cmd, "../mcc", "-r", code);
    cmd_run(&cmd, .stdout_path = out, .stderr_path = out);

    if (!copy_file(out, expect)) return 1;

    return 0;
  }
  UNREACHABLE("test_tool.c");
}

