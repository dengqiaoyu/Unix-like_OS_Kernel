/** @file 410user/progs/idle.c
 *  @author ?
 *  @brief Idle program.
 *  @public yes
 *  @for p2 p3
 *  @covers
 *  @status done
 */

#include <stdio.h>

int main() {
    int i = 0;
    while (1) {
        i++;
        if (i == 100000)
            printf("i : %d\n", i);
    }
}
