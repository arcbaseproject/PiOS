#pragma once
#include "uapi.h"       /* SIG* numbers, SIG_DFL = 0, SIG_IGN = 1 */
typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;
sighandler_t signal(int sig, sighandler_t h);
