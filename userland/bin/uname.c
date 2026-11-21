#include "ulib.h"

int main(int argc, char **argv)
{
    puts(argc > 1 && strcmp(argv[1], "-a") == 0
         ? "PiOS " PIOS_VERSION " aarch64 Raspberry Pi Zero 2 W\n" : "PiOS\n");
    return 0;
}
