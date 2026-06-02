#define NOB_IMPLEMENTATION
#include "3rd/nob.h"

int main(int argc, char **argv)
{
  NOB_GO_REBUILD_URSELF(argc, argv);
  Cmd cmd = {0};
  cmd_append(&cmd, "cc", "-Wall", "-Wextra");
  cmd_append(&cmd, "-O0", "-ggdb");
  cmd_append(&cmd, "-o", "mcc", "mcc.c", "parser.c");
  if (!cmd_run(&cmd)) return 1;
  return 0;
}

