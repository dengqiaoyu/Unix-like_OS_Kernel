/** @file syscalls.h
 *
 */

#ifndef _SYSCALLS_H_
#define _SYSCALLS_H_

int kern_gettid(void);
void kern_exec(void);

#endif