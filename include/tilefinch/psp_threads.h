#ifndef TILEFINCH_PSP_THREADS_H
#define TILEFINCH_PSP_THREADS_H

/*
 * PSP user-thread priorities are inverted: a smaller value runs first.
 * Keep the complete application ladder here; these are architectural
 * orderings established on hardware, not independent local tuning knobs.
 *
 * Callbacks/watchdog must observe lifecycle failures under saturation. DMA
 * staging and audio output drain before codec work. Codec dispatch keeps the
 * Media Engine fed. Transport service/polling outranks the browser because
 * the browser starved curl's bounded 10 ms poll at lower priority. A curl
 * setup perform (DNS/TCP/TLS, before response headers) is temporarily
 * demoted below the browser: libcurl may spend hundreds of milliseconds in
 * that irreducible call on PSP, and chrome/video must remain preemptible.
 * The browser outranks best-effort clock changes for the same reason.
 *
 * Module loading and voice share the short real-time service band, but do not
 * run concurrently with active playback in the supported flow.
 */
#define TILEFINCH_PSP_THREAD_PRIORITY_CALLBACK      0x11
#define TILEFINCH_PSP_THREAD_PRIORITY_WATCHDOG      0x17
#define TILEFINCH_PSP_THREAD_PRIORITY_DMA           0x18
#define TILEFINCH_PSP_THREAD_PRIORITY_MODULE_LOADER 0x18
#define TILEFINCH_PSP_THREAD_PRIORITY_AUDIO         0x18
#define TILEFINCH_PSP_THREAD_PRIORITY_VOICE         0x18
#define TILEFINCH_PSP_THREAD_PRIORITY_CODEC         0x19
#define TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT     0x1f
#define TILEFINCH_PSP_THREAD_PRIORITY_BROWSER       0x20
#define TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT_SETUP 0x21
#define TILEFINCH_PSP_THREAD_PRIORITY_CLOCK         0x21

_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_CALLBACK
                   < TILEFINCH_PSP_THREAD_PRIORITY_WATCHDOG,
               "lifecycle callbacks must outrank the watchdog");
_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_WATCHDOG
                   < TILEFINCH_PSP_THREAD_PRIORITY_BROWSER,
               "the watchdog must outrank ordinary browser work");
_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_DMA
                   < TILEFINCH_PSP_THREAD_PRIORITY_CODEC,
               "DMA staging must drain before the next codec job");
_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_AUDIO
                   < TILEFINCH_PSP_THREAD_PRIORITY_CODEC,
               "audio output must not queue behind firmware decode");
_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_CODEC
                   < TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT,
               "codec dispatch must keep the Media Engine fed");
_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT
                   < TILEFINCH_PSP_THREAD_PRIORITY_BROWSER,
               "transport polling must not be starved by the browser loop");
_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_BROWSER
                   < TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT_SETUP,
               "curl connection setup must remain preemptible by chrome");
_Static_assert(TILEFINCH_PSP_THREAD_PRIORITY_BROWSER
                   < TILEFINCH_PSP_THREAD_PRIORITY_CLOCK,
               "input and chrome must outrank best-effort clock changes");

#endif
