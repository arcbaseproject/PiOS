#pragma once
typedef long jmp_buf[22];       /* x19-x30, sp, d8-d15 */
int setjmp(jmp_buf env);
_Noreturn void longjmp(jmp_buf env, int val);
