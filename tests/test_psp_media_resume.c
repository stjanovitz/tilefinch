#include "../src/psp_media_session_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", \
    __LINE__, #c); return 1; } } while (0)

static bool audible;
static uint64_t cursor;
static unsigned writes;
static int player;

bool test_resume_audio_cursor(const MediaPlayback *playback, uint64_t *us)
{
    if (playback != (const void *)&player || !audible) return false;
    *us = cursor;
    return true;
}

static void changed(void *context, uint64_t now)
{
    (void)context; (void)now;
    ++writes;
}

static void source(PspMediaSession *m, unsigned id)
{
    snprintf(m->source, sizeof(m->source),
             "https://www.youtube.com/watch?v=tf%09u", id);
}

int main(void)
{
    static PspMediaSession m;
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    m.profile = browser_profile_create(&budget);
    CHECK(m.profile != NULL);
    m.platform.profile_changed = changed;
    browser_profile_set_save_playback_positions(m.profile, true);
    for (unsigned i = 0; i < BROWSER_PROFILE_RESUME_LIMIT; ++i) {
        char id[16];
        snprintf(id, sizeof(id), "tf%09u", i);
        CHECK(browser_profile_record_resume(m.profile, id, 20000000, 60000000));
    }
    BrowserProfileResume r;
    source(&m, 0);
    CHECK(!psp_media_record_resume(&m, true)); /* Cancel before resolving. */
    CHECK(browser_profile_resume(m.profile, "tf000000000", &r)
          && r.position_us == 20000000 && r.duration_us == 60000000);
    source(&m, BROWSER_PROFILE_RESUME_LIMIT);
    m.stream.duration_ms = 60000;
    m.playback = (MediaPlayback *)(void *)&player;
    m.have_frame = true; /* Decoding a frame is not presenting it. */
    m.clock_us = 30000000; /* A requested seek isn't evidence either. */
    CHECK(!psp_media_record_resume(&m, true));
    char new_id[16];
    snprintf(new_id, sizeof(new_id), "tf%09u", (unsigned)BROWSER_PROFILE_RESUME_LIMIT);
    CHECK(browser_profile_resume(m.profile, "tf000000000", &r)
          && !browser_profile_resume(m.profile, new_id, &r)
          && writes == 0 && m.last_resume_saved_us == 0);

    source(&m, 0);
    psp_media_note_resume_output(&m, 30000000);
    m.stream.duration_ms = 0;
    CHECK(!psp_media_record_resume(&m, true)); /* Unknown-length stream. */
    m.stream.duration_ms = 60000;
    CHECK(psp_media_record_resume(&m, false) && writes == 0);
    CHECK(browser_profile_resume(m.profile, "tf000000000", &r)
          && r.position_us == 30000000);
    /* Back during a committed forward seek calls the recorder with the
       recovery target as its clock, before any new picture is published. */
    m.job_target_us = m.clock_us = 59000000;
    m.job_phase = PSP_MEDIA_JOB_NONE; /* Close already canceled the seek job. */
    CHECK(psp_media_record_resume(&m, true) && writes == 1);
    CHECK(browser_profile_resume(m.profile, "tf000000000", &r)
          && r.position_us == 30000000);
    /* Reset/prime, and then taking (but not publishing) the first eligible
       picture, still do not supersede the already viewed position. */
    m.have_frame = false;
    m.job_phase = PSP_MEDIA_JOB_SEEK_PRIME;
    CHECK(!psp_media_record_resume(&m, true) && writes == 1);
    m.have_frame = true;
    m.job_phase = PSP_MEDIA_JOB_NONE;
    m.frame.pts_us = 59000000;
    CHECK(!psp_media_record_resume(&m, true) && writes == 1);
    CHECK(browser_profile_resume(m.profile, "tf000000000", &r)
          && r.position_us == 30000000);
    psp_media_note_resume_output(&m, 59000000);
    CHECK(psp_media_record_resume(&m, true) && writes == 2);
    CHECK(browser_profile_resume(m.profile, "tf000000000", &r) && !r.position_us);
    psp_media_note_resume_output(&m, 25000000); /* Seek back after ending. */
    CHECK(psp_media_record_resume(&m, false) && writes == 2);
    m.playback = NULL; /* Retry retired the old decoder before close. */
    CHECK(psp_media_record_resume(&m, true) && writes == 3);
    CHECK(browser_profile_resume(m.profile, "tf000000000", &r)
          && r.position_us == 25000000);

    psp_media_reset_resume_output(&m);
    source(&m, 1);
    m.playback = (MediaPlayback *)(void *)&player;
    m.audio_only = true;
    CHECK(!psp_media_record_resume(&m, true));
    CHECK(browser_profile_resume(m.profile, "tf000000001", &r)
          && r.position_us == 20000000);
    audible = true;
    /* Hardware may report a valid first block at zero. The seek/recovery
       clock still says 59 s, so only the audible cursor amounts to a rewind. */
    CHECK(psp_media_record_resume(&m, true) && writes == 4);
    CHECK(browser_profile_resume(m.profile, "tf000000001", &r)
          && r.position_us == 0 && r.duration_us == 60000000);
    cursor = 40000000;
    CHECK(psp_media_record_resume(&m, false));
    audible = false; /* Reset invalidates the backend's current audio cursor. */
    CHECK(psp_media_record_resume(&m, true) && writes == 5);
    CHECK(browser_profile_resume(m.profile, "tf000000001", &r)
          && r.position_us == 40000000);
    audible = true;
    cursor = 59000000;
    CHECK(psp_media_record_resume(&m, false)); /* Finish during non-persisting poll. */
    CHECK(psp_media_record_resume(&m, true) && writes == 6);
    CHECK(!psp_media_record_resume(&m, true) && writes == 6);
    CHECK(browser_profile_resume(m.profile, "tf000000001", &r) && !r.position_us);

    cursor = 25000000;
    CHECK(psp_media_record_resume(&m, false));
    browser_profile_clear_playback_positions(m.profile);
    psp_media_clear_resume_evidence(&m);
    CHECK(!psp_media_record_resume(&m, true) && writes == 6);
    CHECK(!browser_profile_resume(m.profile, "tf000000001", &r));
    cursor += 1000000;
    CHECK(psp_media_record_resume(&m, true) && writes == 7);
    CHECK(browser_profile_resume(m.profile, "tf000000001", &r)
          && r.position_us == 26000000);

    psp_media_reset_resume_output(&m);
    source(&m, 2);
    m.audio_only = false;
    psp_media_note_resume_output(&m, 21000000);
    CHECK(psp_media_record_resume(&m, false));
    browser_profile_set_save_playback_positions(m.profile, false);
    psp_media_clear_resume_evidence(&m);
    psp_media_note_resume_output(&m, 22000000); /* Playback while disabled. */
    CHECK(!psp_media_record_resume(&m, true) && writes == 7);
    browser_profile_set_save_playback_positions(m.profile, true);
    psp_media_clear_resume_evidence(&m);
    psp_media_note_resume_output(&m, 22000000); /* Redraw, not a new picture. */
    CHECK(!psp_media_record_resume(&m, true) && writes == 7);
    CHECK(!browser_profile_resume(m.profile, "tf000000002", &r));
    psp_media_note_resume_output(&m, 23000000);
    CHECK(psp_media_record_resume(&m, true) && writes == 8);
    CHECK(browser_profile_resume(m.profile, "tf000000002", &r)
          && r.position_us == 23000000);
    browser_profile_destroy(m.profile);
    CHECK(budget.current == 0);
    return 0;
}
