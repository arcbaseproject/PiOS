#include "ulib.h"

int main(int argc, char **argv, char **envp)
{
    puts("[hello] pid ");
    putnum(getpid());
    puts(", argc ");
    putnum(argc);
    puts(":");
    for (int i = 0; i < argc; i++) {
        puts(" ");
        puts(argv[i]);
    }
    for (; *envp; envp++) {
        puts(" env:");
        puts(*envp);
    }
    puts("\n");
    return 42;
}
