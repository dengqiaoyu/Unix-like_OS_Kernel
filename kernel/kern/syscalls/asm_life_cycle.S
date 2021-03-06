/**
 * @file   asm_life_cycle.S
 * @brief  This file contains the assembly function used by fork to copy parent
 *         execution environment to child including setting stack and copy
 *         registers
 * @author Qiaoyu Deng (qdeng)
 * @bug    No known bugs
 */
/* fork */
.global asm_set_exec_context
asm_set_exec_context:
    pushl %ebp              /* save old ebp */
    movl %esp, %ebp         /* %ebp = old %esp */
    pushl %eax              /* save general purpose registers except esp ebp */
    pushl %ecx              /* continue... */
    pushl %edx              /* continue... */
    pushl %ebx              /* continue... */
    pushl %esi              /* continue... */
    pushl %edi              /* continue... */
    movl 20(%ebp), %eax     /* %eax = &(new_thread->ip) */
    pushl %eax              /* push &(new_thread->ip) to stack for future use */
    movl %esp, %eax         /* save parent %esp to %eax */
    movl 8(%ebp), %ecx      /* %ecx = old_kern_sp */
    movl 12(%ebp), %edx     /* %edx = new_kern_sp */
    subl %esp, %ecx         /* len = old_kern_sp - current %esp */
    subl %ecx, %edx         /* new thread %esp = new_kern_sp - len */
    pushl %ecx              /* parameter 3 len */
    pushl %eax              /* parameter 2 old thread esp */
    pushl %edx              /* parameter 1 new thread esp */
    movl 16(%ebp), %eax     /* %eax = &(new_thread->cur_sp) */
    movl %edx, (%eax)       /* *new_thread->cur_sp = new thread %esp */
    call memcpy             /* memcpy(new_thread_sp, old_thread_sp, len) */
    addl $12, %esp          /* restore %esp */
    movl 8(%ebp), %ecx      /* %ecx = old_kern_sp */
    movl 12(%ebp), %edx     /* %edx = new_kern_sp */
    subl %ebp, %ecx         /* len = old_kern_sp - current %esp */
    subl %ecx, %edx         /* new thread %ebp's address = new_kern_sp - len */
    pushl %edx              /* store new thread %ebp's address to stack */
    movl (%ebp), %eax       /* %eax = old %ebp value */
    movl 8(%ebp), %ecx      /* %ecx = old_kern_sp */
    movl 12(%ebp), %edx     /* %edx = new_kern_sp */
    subl %eax, %ecx         /* offset = old_kern_sp - old %ebp value */
    subl %ecx, %edx         /* new thread %ebp's value = new_kern_sp - offset */
    popl %eax               /* new thread %ebp's address */
    movl %edx, (%eax)
    call asm_get_eip        /* get current parent eip, child also run here */
    popl %edx               /* %edx = &(new_thread->ip) */
    movl %eax, (%edx)       /* new_thread->ip = %eip */
    popl %edi               /* restore registers except esp ebp */
    popl %esi               /* continue... */
    popl %ebx               /* continue... */
    popl %edx               /* continue... */
    popl %ecx               /* continue... */
    popl %eax               /* continue... */
    popl %ebp               /* restore old %ebp */
    ret

asm_get_eip:
    movl (%esp), %eax       /* since call int will push eip to stack */
    ret
/* halt */
.global asm_hlt             /* used by kern_halt to halt the system */
asm_hlt:
    HLT

