/* The script split's accounting (include/tilefinch/script_split.h): time
   counts only inside script, timed kinds nest exclusively and restore the
   enclosing kind, a timed kind entered outside script is not counted, polls
   attribute the JS kind by sample, and only the enabling thread counts. A
   fake platform clock makes every interval exact. */
#include "tilefinch/platform.h"
#include "tilefinch/script_split.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                #condition); \
        return 1; \
    } \
} while (0)

static uint64_t fake_now_us;

static uint64_t fake_clock_ns(void *context)
{
    (void) context;
    return fake_now_us * UINT64_C(1000);
}

static void advance(uint64_t us)
{
    fake_now_us += us;
}

static int test_kinds_nest_inside_script_only(void)
{
    ScriptSplitTotals start, end, delta;
    script_split_set_enabled(true);
    script_split_snapshot(&start);
    advance(1000);                                  /* outside script */
    unsigned outside_gc = script_split_enter(SCRIPT_SPLIT_GC);
    advance(300);                                   /* a host collection */
    script_split_leave(outside_gc);
    CHECK(script_split_current() == SCRIPT_SPLIT_KIND_COUNT);
    unsigned js = script_split_enter(SCRIPT_SPLIT_JS);
    advance(100);                                   /* js */
    unsigned style = script_split_enter(SCRIPT_SPLIT_STYLE);
    advance(40);                                    /* computed style */
    unsigned layout = script_split_enter(SCRIPT_SPLIT_LAYOUT);
    advance(25);                                    /* layout it forced */
    unsigned nested = script_split_enter(SCRIPT_SPLIT_JS);
    advance(7);                                     /* script layout ran */
    script_split_leave(nested);
    advance(5);                                     /* layout again */
    script_split_leave(layout);
    CHECK(script_split_current() == SCRIPT_SPLIT_STYLE);
    /* Entering the open kind neither moves time nor counts a call. */
    unsigned same = script_split_enter(SCRIPT_SPLIT_STYLE);
    advance(3);
    script_split_leave(same);
    script_split_leave(style);
    CHECK(script_split_current() == SCRIPT_SPLIT_JS);
    unsigned gc = script_split_enter(SCRIPT_SPLIT_GC);
    advance(60);
    script_split_leave(gc);
    advance(2);
    script_split_leave(js);
    advance(500);                                   /* outside again */
    script_split_snapshot(&end);
    script_split_difference(&end, &start, &delta);
    CHECK(delta.us[SCRIPT_SPLIT_JS] == 109);
    CHECK(delta.us[SCRIPT_SPLIT_STYLE] == 43);
    CHECK(delta.us[SCRIPT_SPLIT_LAYOUT] == 30);
    CHECK(delta.us[SCRIPT_SPLIT_GC] == 60);
    CHECK(delta.calls[SCRIPT_SPLIT_STYLE] == 1);
    CHECK(delta.calls[SCRIPT_SPLIT_LAYOUT] == 1);
    CHECK(delta.calls[SCRIPT_SPLIT_GC] == 1);
    CHECK(delta.us[SCRIPT_SPLIT_FETCH] == 0);
    return 0;
}

static int test_polls_attribute_js_time(void)
{
    ScriptSplitTotals start, end, delta;
    script_split_set_enabled(true);
    script_split_snapshot(&start);
    unsigned js = script_split_enter(SCRIPT_SPLIT_JS);
    advance(70);
    script_split_poll(SCRIPT_SPLIT_SAMPLE_PAGE);     /* 70 us of page */
    advance(20);
    unsigned query = script_split_enter(SCRIPT_SPLIT_QUERY);
    advance(500);                                    /* not JS */
    script_split_leave(query);
    advance(10);
    script_split_poll(SCRIPT_SPLIT_SAMPLE_BOOTSTRAP); /* 30 us */
    advance(4);
    script_split_poll(SCRIPT_SPLIT_SAMPLE_NATIVE);    /* 4 us */
    advance(9);                                       /* not yet polled */
    script_split_leave(js);
    script_split_snapshot(&end);
    script_split_difference(&end, &start, &delta);
    CHECK(delta.us[SCRIPT_SPLIT_JS] == 113);
    CHECK(delta.us[SCRIPT_SPLIT_QUERY] == 500);
    CHECK(delta.sampled_us[SCRIPT_SPLIT_SAMPLE_PAGE] == 70);
    CHECK(delta.sampled_us[SCRIPT_SPLIT_SAMPLE_BOOTSTRAP] == 30);
    CHECK(delta.sampled_us[SCRIPT_SPLIT_SAMPLE_NATIVE] == 4);
    CHECK(delta.polls == 3);
    char text[SCRIPT_SPLIT_FORMAT_BYTES];
    int length = script_split_format(&delta, text, sizeof(text));
    CHECK(length > 0 && (size_t) length == strlen(text));
    CHECK(strstr(text, "js=113 ") == text);
    CHECK(strstr(text, " query=500 ") != NULL);
    CHECK(strstr(text, " page=70 bootstrap=30 native=4 polls=3") != NULL);
    CHECK(strstr(text, " query-calls=1") != NULL);
    /* A short buffer truncates, terminated. */
    char small[16];
    length = script_split_format(&delta, small, sizeof(small));
    CHECK(length == 15 && small[15] == '\0');
    return 0;
}

static unsigned foreign_token;

static void *foreign_thread(void *unused)
{
    (void) unused;
    /* A site reached from another thread (a transport worker) must not
       move the owner's time between kinds. */
    foreign_token = script_split_enter(SCRIPT_SPLIT_FETCH);
    advance(50);
    script_split_leave(foreign_token);
    return NULL;
}

static int test_only_the_enabling_thread_counts(void)
{
    ScriptSplitTotals start, end;
    script_split_set_enabled(true);
    script_split_snapshot(&start);
    unsigned js = script_split_enter(SCRIPT_SPLIT_JS);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, foreign_thread, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    script_split_leave(js);
    script_split_snapshot(&end);
    CHECK(end.us[SCRIPT_SPLIT_JS] - start.us[SCRIPT_SPLIT_JS] == 50);
    CHECK(end.us[SCRIPT_SPLIT_FETCH] == start.us[SCRIPT_SPLIT_FETCH]);
    CHECK(end.calls[SCRIPT_SPLIT_FETCH] == start.calls[SCRIPT_SPLIT_FETCH]);
    return 0;
}

static int test_disabled_split_counts_nothing(void)
{
    ScriptSplitTotals start, end;
    script_split_set_enabled(true);
    unsigned open = script_split_enter(SCRIPT_SPLIT_JS);
    script_split_set_enabled(false);
    script_split_snapshot(&start);
    unsigned token = script_split_enter(SCRIPT_SPLIT_STYLE);
    advance(500);
    script_split_poll(SCRIPT_SPLIT_SAMPLE_PAGE);
    script_split_leave(token);
    script_split_leave(open);
    script_split_snapshot(&end);
    CHECK(memcmp(&start, &end, sizeof(start)) == 0);
    /* Re-enabling starts outside script: a token from before is inert. */
    script_split_set_enabled(true);
    CHECK(script_split_current() == SCRIPT_SPLIT_KIND_COUNT);
    script_split_leave(open);
    CHECK(script_split_current() == SCRIPT_SPLIT_KIND_COUNT);
    script_split_set_enabled(false);
    return 0;
}

static int test_sampling_off_keeps_timed_kinds(void)
{
    ScriptSplitTotals start, end, delta;
    script_split_set_enabled(true);
    script_split_set_sampling(false);
    CHECK(!script_split_sampling());
    script_split_snapshot(&start);
    unsigned js = script_split_enter(SCRIPT_SPLIT_JS);
    advance(40);
    script_split_poll(SCRIPT_SPLIT_SAMPLE_PAGE);    /* not sampled */
    unsigned style = script_split_enter(SCRIPT_SPLIT_STYLE);
    advance(15);
    script_split_leave(style);
    advance(5);
    script_split_leave(js);
    script_split_snapshot(&end);
    script_split_difference(&end, &start, &delta);
    CHECK(delta.us[SCRIPT_SPLIT_JS] == 45);
    CHECK(delta.us[SCRIPT_SPLIT_STYLE] == 15);
    CHECK(delta.polls == 0);
    CHECK(delta.sampled_us[SCRIPT_SPLIT_SAMPLE_PAGE] == 0);
    script_split_set_sampling(true);
    CHECK(script_split_sampling());
    return 0;
}

int main(void)
{
    TilefinchPlatformServices services = {
        .monotonic_time_ns = fake_clock_ns
    };
    tilefinch_platform_set_services(&services);
    fake_now_us = 1000000;
    CHECK(test_kinds_nest_inside_script_only() == 0);
    CHECK(test_polls_attribute_js_time() == 0);
    CHECK(test_only_the_enabling_thread_counts() == 0);
    CHECK(test_disabled_split_counts_nothing() == 0);
    CHECK(test_sampling_off_keeps_timed_kinds() == 0);
    tilefinch_platform_set_services(NULL);
    puts("script split tests passed");
    return 0;
}
