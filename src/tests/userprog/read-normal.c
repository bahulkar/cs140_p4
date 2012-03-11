/* Try reading a file in the most normal way. */

#include "tests/userprog/sample.inc"
#include "tests/lib.h"
#include "tests/main.h"
#include <stdio.h>

void
test_main (void) 
{
  printf ("***test\n");
  check_file ("sample.txt", sample, sizeof sample - 1);
}
