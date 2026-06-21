#define NOB_IMPLEMENTATION
#include "3rd/nob.h"

typedef struct {
  Cmd   *cmd;
  Procs *async;
} Cmd_Async;

bool test = false;
bool update_test = false;
bool run_test(Nob_Walk_Entry entry) {
  if (entry.type == FILE_DIRECTORY) return true;

  Cmd_Async *data  = entry.data;
  Cmd   *cmd       = data->cmd;
  Procs *async     = data->async;

  size_t mark = temp_save();
  char *filename = temp_file_name(entry.path);

  cmd_append(cmd, "./test_tool");
  if (test) {
    cmd_append(cmd, "test");
  } else if (update_test) {
    cmd_append(cmd, "update");
  }
  cmd_append(cmd, filename);
  cmd_run(cmd, .async = async);
  temp_rewind(mark);

  return true;
}

char *self;
void usage(FILE *stream)
{
  fprintf(stream, "Usage: %s [update|update-test]", self);
}

int main(int argc, char **argv)
{
  NOB_GO_REBUILD_URSELF(argc, argv);


  self = nob_shift(argv, argc);

  if (argc != 0) {
    char *command = shift(argv, argc);
    if (strcmp(command, "test") == 0) {
      test = true;
    } else if (strcmp(command, "update-test") == 0) {
      update_test = true;
    } else if (strcmp(command, "help")) {
      usage(stdout);
      return 0;
    } else {
      usage(stderr);
      return 1;
    }
  }

  if (argc != 0) {
    usage(stderr);
    return 1;
  }

  Cmd cmd = {0};
  cmd_append(&cmd, "cc", "-Wall", "-Wextra");
  cmd_append(&cmd, "-O0", "-ggdb");
  cmd_append(&cmd, "-I./3rd");
  cmd_append(&cmd, "-o", "mcc", "src/mcc.c", "src/parser.c");
  if (!cmd_run(&cmd)) return 1;

  if (test || update_test) {
    cmd_append(&cmd, "cc", "-Wall", "-Wextra");
    cmd_append(&cmd, "-O2", "-g0");
    cmd_append(&cmd, "-o", "test_tool", "test_tool.c");
    if (!cmd_run(&cmd)) {
      nob_log(NOB_ERROR, "failed to build test_tool");
      return 1;
    }

    if (!mkdir_if_not_exists("./build/")) {
      nob_log(NOB_ERROR, "failed to create ./build/ directory");
      return 1;
    }

    Procs async = {0};
    Cmd_Async data = {
      .cmd   = &cmd,
      .async = &async,
    };
    if (!walk_dir("./example/", run_test, .data = &data)) return 1;
    if (!procs_wait_and_reset(&async)) return 1;
  }

  return 0;
}

