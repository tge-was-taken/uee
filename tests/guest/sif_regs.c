#include <kernel.h>
#include <stdio.h>

#include "syscall.h"

enum {
    kSifStopDma = 107,
    kSifSetDChain = 120,
    kSifSetReg = 121,
    kSifGetReg = 122,
};

enum {
    kMainAddr = (int)0x80000000,
    kSubAddr = (int)0x80000001,
    kMsFlag = (int)0x80000002,
    kOutOfRange = (int)0x80000020,
};

int main(void)
{
    int mscom, msflg, smflg, written;

    printf("-- TEST BEGIN\n");

    // The software register file backs the RPC handshake slots and is plain
    // storage; only the low five bits index it and 0x20 is past the end.
    syscall2(kSifSetReg, kMainAddr, 0x11223344);
    syscall2(kSifSetReg, kSubAddr, 0x55667788);
    syscall2(kSifSetReg, kMsFlag, 0x99AABBCC);
    printf("software regs: %08x %08x %08x\n", syscall1(kSifGetReg, kMainAddr),
           syscall1(kSifGetReg, kSubAddr), syscall1(kSifGetReg, kMsFlag));

    syscall2(kSifSetReg, kOutOfRange, 0xDEADBEEF);
    printf("index 0x20 dropped: %08x\n", syscall1(kSifGetReg, kOutOfRange));

    syscall2(kSifSetReg, 5, 0xDEADBEEF);
    printf("reg 5 ignored: %08x\n", syscall1(kSifGetReg, 5));
    printf("reg 0 ignored: %08x\n", syscall1(kSifGetReg, 0));

    // Register 1 is the EE-to-IOP mailbox and reads back exactly what was put
    // there; the write answers with that read-back value.
    written = syscall2(kSifSetReg, 1, 0x0BADF00D);
    mscom = syscall1(kSifGetReg, 1);
    printf("mscom: write %08x, read %08x\n", written, mscom);

    // Each side may only set bits in its own flag word, so writes accumulate.
    syscall2(kSifSetReg, 3, 0x00001234);
    msflg = syscall1(kSifGetReg, 3);
    syscall2(kSifSetReg, 3, 0x00000F00);
    printf("msflg: first %08x, merged %08x\n", msflg, syscall1(kSifGetReg, 3));

    // The EE may only clear bits in the IOP's flag word, so a write cannot
    // raise one.
    smflg = syscall1(kSifGetReg, 4);
    syscall2(kSifSetReg, 4, 0x0000FFFF);
    printf("smflg unraised: %d\n", (syscall1(kSifGetReg, 4) & ~smflg) == 0 ? 1 : 0);
    printf("smcom readable: %d\n", syscall1(kSifGetReg, 2) != (int)0xFFFFFFFF);

    printf("set dchain: %03x\n", syscall0(kSifSetDChain));
    printf("stop dma: %d\n", syscall0(kSifStopDma));

    printf("-- TEST END\n");
    return 0;
}
