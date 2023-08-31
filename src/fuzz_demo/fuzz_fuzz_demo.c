#if !FD_HAS_HOSTED
#error "This target requires FD_HAS_HOSTED"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../util/fd_util.h"
#include "fd_fuzz_demo.h"

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  /* Set up shell without signal handlers */
  putenv("FD_LOG_BACKTRACE=0");
  fd_boot(argc, argv);
  atexit(fd_halt);

  /* Disable parsing error logging */
  fd_log_level_stderr_set(4);
  return 0;
}

int LLVMFuzzerTestOneInput(uchar const *data, ulong size) {
  fd_fuzz_broken_fn((uchar *)data, size);
  return 0;
}
