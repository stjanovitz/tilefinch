/* tfdeploy -- a development-only, low-wear fixed-artifact deployer for PSPLink.
 *
 * PSPLink's built-in `cp` copies through a 2 KiB shell buffer. For the
 * roughly 6 MiB Tilefinch EBOOT that means thousands of USB round trips.
 * This PRX instead reads from host0 in 1 MiB units and writes a temporary
 * file in at most eight payload writes before atomically promoting it.
 *
 * The ordinary Tilefinch edit loop should still load the browser PRX from
 * host0 and perform zero Memory Stick writes. This helper exists only when
 * installed-slot paths or launcher behavior must be exercised.
 */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("tfdeploy", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
/* Never let a short-lived transfer helper reserve all remaining user RAM. */
PSP_HEAP_SIZE_KB(64);

/* The custom self-unload below bypasses crt0's ordinary _exit teardown. */
extern void __libcglue_deinit(void);

#define TF_DEPLOY_CHUNK_BYTES (1024u * 1024u)
#define TF_DEPLOY_MAX_BYTES (8u * 1024u * 1024u)
#define TF_DEPLOY_PATH_BYTES 256u

typedef char TfDeployWriteBound[
    ((TF_DEPLOY_MAX_BYTES + TF_DEPLOY_CHUNK_BYTES - 1u) /
     TF_DEPLOY_CHUNK_BYTES) <= 8u ? 1 : -1];

static const char default_source[] = "host0:/EBOOT-device-latest.PBP";
static const char default_destination[] =
    "ms0:/PSP/GAME/TILEFINCH/slot-a/EBOOT.PBP";
static const char result_path[] = "host0:/tfdeploy.result";

/* Static storage keeps the 1 MiB transfer unit off PSPLink's small module
   thread stack. Cache-line alignment is useful for the USB and Memory Stick
   drivers even though neither device is accessed by the Media Engine. */
static unsigned char transfer_buffer[TF_DEPLOY_CHUNK_BYTES]
    __attribute__((aligned(64)));

static void publish_result(const char *status,
                           const char *phase,
                           int native_result,
                           unsigned int bytes,
                           unsigned int writes)
{
    char message[192];
    int length = snprintf(message,
                          sizeof(message),
                          "status=%s phase=%s native=0x%08X bytes=%u writes=%u\n",
                          status,
                          phase,
                          (unsigned int) native_result,
                          bytes,
                          writes);
    if (length < 0) return;
    if ((size_t) length >= sizeof(message)) length = (int) sizeof(message) - 1;

    SceUID file = sceIoOpen(result_path,
                            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                            0666);
    if (file >= 0) {
        (void) sceIoWrite(file, message, (unsigned int) length);
        (void) sceIoClose(file);
    }
    printf("tfdeploy: %s", message);
}

static int finish(const char *status,
                  const char *phase,
                  int native_result,
                  unsigned int bytes,
                  unsigned int writes,
                  int exit_status)
{
    publish_result(status, phase, native_result, bytes, writes);
    /* Match crt0's runtime teardown before self-unload; otherwise newlib's
       partition allocation outlives the module. No libc call follows. */
    __libcglue_deinit();
    (void) sceKernelSelfStopUnloadModule(1, 0, NULL);
    /* A refused self-unload leaves a stopped module for the host to unload,
       never a return through crt0 that would deinitialize libc twice. */
    (void) sceKernelExitDeleteThread(exit_status);
    return exit_status;
}

static int path_with_suffix(char *output,
                            size_t output_size,
                            const char *path,
                            const char *suffix)
{
    int written = snprintf(output, output_size, "%s%s", path, suffix);
    return written > 0 && (size_t) written < output_size;
}

static int write_unit(SceUID file,
                      const unsigned char *data,
                      unsigned int size,
                      unsigned int *write_calls)
{
    int written = sceIoWrite(file, data, size);
    *write_calls += 1u;
    if (written < 0) return written;
    /* A short write fails the transaction instead of issuing more writes.
       This makes the <=8 Memory Stick payload-write bound unconditional. */
    return (unsigned int) written == size ? 0 : -1;
}

static int read_unit(SceUID file, unsigned char *data, unsigned int size)
{
    unsigned int offset = 0;
    while (offset < size) {
        int received = sceIoRead(file, data + offset, size - offset);
        if (received <= 0) return received < 0 ? received : -1;
        offset += (unsigned int) received;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *source = default_source;
    const char *destination = default_destination;
    char temporary[TF_DEPLOY_PATH_BYTES];
    char previous[TF_DEPLOY_PATH_BYTES];
    SceIoStat source_stat;
    SceIoStat installed_stat;
    SceIoStat temporary_stat;
    SceUID source_file = -1;
    SceUID destination_file = -1;
    unsigned int copied = 0;
    unsigned int write_calls = 0;
    int native_result = 0;
    int destination_existed = 0;
    const char *phase = "arguments";

    int is_prx = 0;
    if (argc == 2 && strcmp(argv[1], "wasm") == 0) {
        source = "host0:/tilefinch-wasm-device-latest.prx";
        destination = "ms0:/PSP/GAME/TILEFINCH/slot-a/tilefinch-wasm.prx";
        is_prx = 1;
    } else if (argc == 2 && strcmp(argv[1], "voice") == 0) {
        source = "host0:/tilefinch-voice-device-latest.prx";
        destination = "ms0:/PSP/GAME/TILEFINCH/slot-a/tilefinch-voice.prx";
        is_prx = 1;
    } else if (argc > 1 && (argc != 2 || strcmp(argv[1], "eboot") != 0)) {
        return finish("error", phase, -1, 0, 0, 1);
    }

    /* Fixed paths keep this from becoming a general-purpose remote overwrite
       primitive. */
    if (!path_with_suffix(temporary, sizeof(temporary), destination, ".new") ||
        !path_with_suffix(previous, sizeof(previous), destination, ".previous")) {
        return finish("error", phase, -1, 0, 0, 1);
    }

    phase = "source-stat";
    memset(&source_stat, 0, sizeof(source_stat));
    native_result = sceIoGetstat(source, &source_stat);
    if (native_result < 0 || source_stat.st_size <= 0 ||
        source_stat.st_size > (SceOff) TF_DEPLOY_MAX_BYTES) {
        return finish("error", phase, native_result, 0, 0, 1);
    }

    phase = "source-open";
    source_file = sceIoOpen(source, PSP_O_RDONLY, 0);
    if (source_file < 0) {
        return finish("error", phase, source_file, 0, 0, 1);
    }

    /* Repair the only interruption window in the promotion sequence: an
       earlier run may have retained the installed EBOOT as .previous but
       lost power before giving the new file its final name. */
    memset(&installed_stat, 0, sizeof(installed_stat));
    if (sceIoGetstat(destination, &installed_stat) < 0) {
        memset(&temporary_stat, 0, sizeof(temporary_stat));
        if (sceIoGetstat(previous, &temporary_stat) >= 0) {
            phase = "recover-previous";
            native_result = sceIoRename(previous, destination);
            if (native_result < 0) goto fail;
            native_result = sceIoSync("ms0:", 0);
            if (native_result < 0) goto fail;
        }
    }

    /* A stale .new is never authoritative and is safe to discard. */
    (void) sceIoRemove(temporary);
    phase = "temporary-open";
    destination_file = sceIoOpen(temporary,
                                 PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                                 0777);
    if (destination_file < 0) {
        native_result = destination_file;
        goto fail;
    }

    phase = "copy";
    while (copied < (unsigned int) source_stat.st_size) {
        unsigned int remaining = (unsigned int) source_stat.st_size - copied;
        unsigned int request = remaining < TF_DEPLOY_CHUNK_BYTES
            ? remaining : TF_DEPLOY_CHUNK_BYTES;
        native_result = read_unit(source_file, transfer_buffer, request);
        if (native_result < 0) goto fail;
        if (copied == 0 && (request < 4u ||
            (is_prx ? memcmp(transfer_buffer, "\177ELF", 4u) != 0
                    : memcmp(transfer_buffer, "\0PBP", 4u) != 0))) {
            native_result = -1;
            phase = "artifact-header";
            goto fail;
        }
        native_result = write_unit(destination_file,
                                   transfer_buffer,
                                   request,
                                   &write_calls);
        if (native_result < 0) goto fail;
        copied += request;
    }

    phase = "temporary-close";
    native_result = sceIoClose(destination_file);
    destination_file = -1;
    if (native_result < 0) goto fail;
    native_result = sceIoClose(source_file);
    source_file = -1;
    if (native_result < 0) goto fail;
    native_result = sceIoSync("ms0:", 0);
    if (native_result < 0) {
        phase = "temporary-sync";
        goto fail;
    }

    phase = "temporary-verify";
    memset(&temporary_stat, 0, sizeof(temporary_stat));
    native_result = sceIoGetstat(temporary, &temporary_stat);
    if (native_result < 0 || temporary_stat.st_size != source_stat.st_size ||
        copied != (unsigned int) source_stat.st_size || write_calls > 8u) {
        if (native_result >= 0) native_result = -1;
        goto fail;
    }

    /* Preserve the old slot until the complete temporary file is visible.
       Rename operations update FAT metadata but do not rewrite the payload. */
    (void) sceIoRemove(previous);
    memset(&installed_stat, 0, sizeof(installed_stat));
    native_result = sceIoGetstat(destination, &installed_stat);
    destination_existed = native_result >= 0;
    if (destination_existed) {
        phase = "retain-previous";
        native_result = sceIoRename(destination, previous);
        if (native_result < 0) goto fail;
    }

    phase = "promote";
    native_result = sceIoRename(temporary, destination);
    if (native_result < 0) {
        if (destination_existed) (void) sceIoRename(previous, destination);
        goto fail;
    }
    native_result = sceIoSync("ms0:", 0);
    if (native_result < 0) {
        phase = "promote-sync";
        /* The promoted file is complete even if persistence could not be
           confirmed; keep the previous copy for manual recovery. */
        return finish("error",
                      phase,
                      native_result,
                      copied,
                      write_calls,
                      1);
    }
    if (destination_existed) (void) sceIoRemove(previous);
    return finish("ok", "complete", 0, copied, write_calls, 0);

fail:
    if (destination_file >= 0) (void) sceIoClose(destination_file);
    if (source_file >= 0) (void) sceIoClose(source_file);
    (void) sceIoRemove(temporary);
    return finish("error", phase, native_result, copied, write_calls, 1);
}
