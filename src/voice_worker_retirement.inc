/* Owner-side firmware cleanup, also compiled by the host refusal tests.
   Completion notification, joining and deleting are separate obligations. */
typedef struct {
    int thread;
    int event;
    bool started;
    bool joined;
} VoiceWorkerResources;

static int voice_worker_retire(VoiceWorkerResources *worker)
{
    if (worker->started && !worker->joined) {
        int result = psp_thread_wait_end_bounded(worker->thread, 500000u);
        if (result < 0) return result;
        worker->joined = true;
    }
    if (worker->thread >= 0) {
        int result = sceKernelDeleteThread(worker->thread);
        if (result < 0) return result;
        worker->thread = -1;
    }
    if (worker->event >= 0) {
        int result = sceKernelDeleteEventFlag(worker->event);
        if (result < 0) return result;
        worker->event = -1;
    }
    return 0;
}
