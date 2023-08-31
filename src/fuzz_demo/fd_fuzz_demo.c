
#include "fd_fuzz_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../util/log/fd_log.h"

int fd_fuzz_broken_fn(uchar *buf, ulong len) {

  if (FD_UNLIKELY(len < 3)) {
    return 0;
  }

  if (buf[0] == 'F')
    if (buf[1] == 'U')
      if (buf[2] == 'Z')
        if (buf[3] == 'Z') // This access should sometime crash
          FD_LOG_INFO(("Success"));

  return 0;
}
