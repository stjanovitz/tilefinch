/* Media Engine boot stub and exception handlers (templates copied into the
   ME's boot area at 0xBFC00000 by the CPU, immediates patched at run time). */
    .set noreorder
    .set noat

    .global me_stub, me_stub_end
    .ent me_stub
me_stub:                          /* placed at 0xBFC00040: sp, entry, box */
    lui   $sp, 0x1234             /* [0] patched: stack top (uncached) */
    ori   $sp, $sp, 0x5678        /* [1] */
    lui   $t9, 0x1234             /* [2] patched: me_main */
    ori   $t9, $t9, 0x5678        /* [3] */
    lui   $a0, 0x1234             /* [4] patched: mailbox (uncached) */
    ori   $a0, $a0, 0x5678        /* [5] */
    jr    $t9
    nop
me_stub_end:
    .end me_stub

    .global me_exc, me_exc_end
    .ent me_exc
me_exc:                           /* placed at 0xBFC00200 and 0xBFC00380 */
    lui   $k0, 0x1234             /* [0] patched: mailbox (uncached) */
    ori   $k0, $k0, 0x5678        /* [1] */
    mfc0  $k1, $13                /* cause */
    sw    $k1, 32($k0)            /* box->fault_cause */
    mfc0  $k1, $14                /* epc */
    sw    $k1, 36($k0)            /* box->fault_epc */
    li    $k1, 0xDEAD
    sw    $k1, 28($k0)            /* box->fault */
1:  b     1b
    nop
me_exc_end:
    .end me_exc
