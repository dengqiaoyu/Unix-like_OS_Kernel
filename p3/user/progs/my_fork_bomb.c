/** @file 410user/progs/fork_bomb.c
 *  @author zra
 *  @brief Tests fork() in low-memory conditions.
 *  @public yes
 *  @for p3
 *  @covers fork oom
 *  @status done
 */

/* Includes */
#include <syscall.h>    /* for fork */
#include <stdlib.h>
#include <simics.h>
#include "410_tests.h"
#include <report.h>

DEF_TEST_NAME("fork_bomb:");

/* Main */
int main() {
    report_start(START_4EVER);
    TEST_PROG_ENGAGE(200);

    while (1) {
        int pid = fork();
        if (pid == 0) lprintf("Hi I am task %d\n", gettid());
        // TEST_PROG_PROGRESS;
    }
}