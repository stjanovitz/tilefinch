/* Named, opt-in PSP gate. No microphone/audio output; never linked into the
 * browser. Stage tilefinch-voice.prx and voice-model beside this EBOOT. */
#include "stt_component_loader.h"
#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

PSP_MODULE_INFO("TilefinchVoiceProbe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-1024);
static int16_t silence[STT_ENGINE_CAPTURE_RATE];
static int cancel(void *u) { (void)u; return 1; }
static uint64_t now(void *u) { (void)u; return sceKernelGetSystemTimeWide(); }

int main(int argc, char **argv)
{
    char directory[768] = ".";
    if (argc && argv[0]) {
        snprintf(directory, sizeof(directory), "%s", argv[0]);
        char *end = strrchr(directory, '/');
        if (end) *end = '\0';
        else strcpy(directory, ".");
    }
    chdir(directory);
    if (remove("voice-probe.txt") != 0 && errno != ENOENT) {
        sceKernelExitGame(); return 1;
    }
    FILE *log = fopen("voice-probe.txt", "w");
    if (!log) { sceKernelExitGame(); return 1; }
    Budget budget;
    budget_init(&budget, 24u * 1024u * 1024u);
    psp_voice_component_configure(directory);
    fprintf(log, "voice-probe: configured=%s\n", directory);
    fflush(log);
    int okay = 1;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        size_t before = mallinfo().uordblks;
        uint64_t started = now(NULL);
        if (!psp_voice_component_load(&budget)) {
            fprintf(log, "cycle=%u load=failed\n", cycle);
            okay = 0; break;
        }
        fprintf(log, "cycle=%u loaded=yes\n", cycle);
        fflush(log);
        SttEngineConfig config;
        stt_engine_config_init(&config);
        config.acoustic_model_path = "voice-model/en-us";
        config.dictionary_path = "voice-model/search/search.dict";
        config.language_model_path = "voice-model/search/search.lm.bin";
        config.clock_microseconds = now;
        SttEngine *engine = NULL;
        fprintf(log, "cycle=%u create=begin\n", cycle);
        fflush(log);
        SttStatus status = stt_engine_create(&engine, &config);
        fprintf(log, "cycle=%u create=%d engine=%d init-us=%llu\n", cycle,
                status, engine != NULL, (unsigned long long)(now(NULL)-started));
        if (status != STT_STATUS_OK || !engine) okay = 0;
        if (engine) {
            SttResult result;
            status = stt_engine_decode_capture_pcm_cancelable(
                engine, silence, STT_ENGINE_CAPTURE_RATE, cancel, NULL, &result);
            fprintf(log, "cycle=%u cancellation=%d\n", cycle, status);
            if (status != STT_STATUS_CANCELLED) okay = 0;
            stt_engine_destroy(engine);
        }
        psp_voice_component_unload();
        size_t after = mallinfo().uordblks;
        fprintf(log, "cycle=%u owned=%zu heap-before=%zu heap-after=%zu\n",
                cycle, budget.current, before, after);
        if (budget.current != 0) okay = 0;
        if (cycle != 0 && after != before) okay = 0;
        fflush(log);
    }
    fprintf(log, "tilefinch-voice-probe: outcome=%s\n", okay ? "pass" : "fail");
    fclose(log);
    sceKernelExitGame();
    return !okay;
}
