/* Executes the fp-asm test, then computes an approximation of e itself
   (as a userprog) simultaneously. The fp-asm program itself checks 
   whether the different exec'd processes' FPU registers interfere with
   each other. Additionally, we want to check if the fp-asm's program 
   interferes with computing the value of e. Not only do we ensure
   that floating point registers are saved on a context switch, but we
   also test whether our Floating Point functionality works with more
   than two processes. */

#include <float.h>
#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

const char* test_name = "fp-asm-e";

void test_main(void) {
  msg("Computing e...");
  pid_t asm_pid = exec("fp-asm");
  double e_res = sum_to_e(10);
  wait(asm_pid);
  if (abs(e_res - E_VAL) < TOL) {
    msg("Success!");
    exit(162);
  } else {
    msg("Got e=%f, expected e=%f", e_res, E_VAL);
    exit(126);
  }
}
