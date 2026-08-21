/* swdec-meload: tiny kernel-mode helper that boots the Media Engine on a
   caller-supplied entry point (RAM only, nothing persistent).
   User-callable exports (syscalls): swdec_me_boot, swdec_me_stop, swdec_me_fault.
   The firmware's own ME boot preamble at 0xBFC00060..0x110 is kept and only
   its constants are patched (a0 = arg, ra = entry, sp = stack). */
#include <pspkernel.h>
#include <pspsysreg.h>
#include <psputils.h>
#include <pspsdk.h>
#include <string.h>

PSP_MODULE_INFO("swdec_meload", 0x1006, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

extern char me_exc[], me_exc_end[];

typedef struct { volatile unsigned cmd, arg0, arg1, res0, res1, heartbeat, done, fault, fault_cause, fault_epc, pad[6]; } MeFaultBox;
static MeFaultBox faultbox __attribute__((aligned(64)));
static int me_running;

#define UNC(p) ((unsigned) (((unsigned) (p) & 0x1FFFFFFFu) | 0xA0000000u))
#define KSEG0(p) ((unsigned) (((unsigned) (p) & 0x1FFFFFFFu) | 0x80000000u))

/* snapshot of the ME boot area (0xBFC00000..+0x400 = 256 words) taken before
   the first patch, so the firmware ME image can be restored byte-for-byte */
static unsigned me_boot_snapshot[256];
static int me_boot_saved;

int swdec_me_boot(unsigned entry, unsigned arg, unsigned stack_top)
{
    volatile unsigned *ba = (volatile unsigned *) 0xBFC00000u;
    unsigned k1 = pspSdkSetK1(0);
    entry = KSEG0(entry);
    /* sanity: the preamble must still be the firmware's (li t1,2 at +0x60 or our restored one; jr ra at +0x10c) */
    if (ba[0x43] != 0x03e00008u) { pspSdkSetK1(k1); return -1; }
    if (!me_boot_saved) {
        for (int i = 0; i < 256; i++) me_boot_snapshot[i] = ba[i];
        me_boot_saved = 1;
    }
    unsigned box = UNC(&faultbox);
    memset(&faultbox, 0, sizeof faultbox);
    for (int i = 0x10; i < 0x18; i++) ba[i] = 0;                       /* +0x40..+0x5c: fall through */
    ba[0x18] = 0x24090002u;                                             /* li t1,2 */
    ba[0x19] = 0x3c040000u | (arg >> 16);   ba[0x1a] = 0x34840000u | (arg & 0xFFFFu);       /* a0 = arg */
    ba[0x40] = 0x3c1f0000u | (entry >> 16); ba[0x41] = 0x37ff0000u | (entry & 0xFFFFu);     /* ra = entry */
    ba[0x42] = 0x3c1d0000u | (stack_top >> 16);                                             /* lui sp */
    ba[0x44] = 0x37bd0000u | (stack_top & 0xFFFFu);                                         /* ori sp (jr delay slot) */
    unsigned exc[16];
    memcpy(exc, me_exc, me_exc_end - me_exc);
    exc[0] = (exc[0] & 0xFFFF0000u) | (box >> 16); exc[1] = (exc[1] & 0xFFFF0000u) | (box & 0xFFFF);
    memcpy((void *) 0xBFC00200u, exc, me_exc_end - me_exc);
    memcpy((void *) 0xBFC00380u, exc, me_exc_end - me_exc);
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    sceSysregMeResetEnable();
    sceSysregMeBusClockEnable();
    sceSysregMeResetDisable();
    sceSysregVmeResetDisable();
    me_running = 1;
    pspSdkSetK1(k1);
    return 0;
}

int swdec_me_stop(void)
{
    unsigned k1 = pspSdkSetK1(0);
    if (me_running) { sceSysregMeResetEnable(); me_running = 0; }
    pspSdkSetK1(k1);
    return 0;
}

/* Restore the firmware ME image: put the ME in reset, write the saved boot
   area back, and re-run the firmware reset sequence so the next
   sceAudiocodec/sceMpeg reboots the ME exactly as if we never patched it.
   Returns 0 on restore, 1 if nothing was ever saved (already pristine). */
int swdec_me_restore(void)
{
    volatile unsigned *ba = (volatile unsigned *) 0xBFC00000u;
    unsigned k1 = pspSdkSetK1(0);
    if (!me_boot_saved) { pspSdkSetK1(k1); return 1; }
    sceSysregMeResetEnable();                 /* hold the ME in reset */
    for (int i = 0; i < 256; i++) ba[i] = me_boot_snapshot[i];
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    /* release the reset so the ORIGINAL firmware image boots: the ME comes up
       running its own dispatcher, exactly the state sceAudiocodec/sceMpeg
       expect after a normal system boot */
    sceSysregMeResetDisable();
    sceSysregVmeResetDisable();
    me_running = 0;
    me_boot_saved = 0;                         /* boot area is pristine again */
    pspSdkSetK1(k1);
    return 0;
}
int module_stop(SceSize args, void *argp);

/* returns fault marker (0 = none, 0xDEAD = exception); cause/epc through pointers */
int swdec_me_fault(unsigned *cause, unsigned *epc)
{
    unsigned k1 = pspSdkSetK1(0);
    volatile MeFaultBox *b = (volatile MeFaultBox *) UNC(&faultbox);
    if (cause) *cause = b->fault_cause;
    if (epc) *epc = b->fault_epc;
    int f = (int) b->fault;
    pspSdkSetK1(k1);
    return f;
}

int module_start(SceSize args, void *argp) { (void) args; (void) argp; return 0; }
int module_stop(SceSize args, void *argp) { (void) args; (void) argp; swdec_me_stop(); return 0; }
