#include "i386.h"
#include <stdio.h>
#include <stdlib.h>

/* Native BIOS functions are installed here as they are ported to Linux.
 * An absent handler is a fatal integration error, not a successful IRET. */
handler_t handlers[256];

bool native_bios_dispatch(CPU *cpu, uint8_t intnum)
{
    handler_t handler = handlers[intnum];
    if (!handler) {
        fprintf(stderr,
                "orange386: native BIOS handler %02Xh is not installed\n",
                (unsigned)intnum);
        abort();
    }
    return handler(cpu);
}
