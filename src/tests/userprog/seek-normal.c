/* Part 3 of the project required us to implement some file system calls. 
   The test-main will check whether seek can adjust the file pointer to 
   the correct offset. We test this by using tell and check whether the 
   position it returns 2. If it does, then we know that there was no write
   to the file because we already seeked to 2 by using seek(fd, 2).  */

#include "tests/lib.h"
#include "tests/main.h"
#include <stdlib.h>

void test_main(void) {
  // Try to open the running file
  int fd = open("sample.txt");
  seek(fd, 2);
  int pos = tell(fd);
  if (pos == 2) {
    msg("PASS - Sought to tell!");
  } else {
    fail("should have exited with -1");
  }
}
