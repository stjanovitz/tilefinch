/* The validation entropy capture's pure half (src/psp_entropy_capture.h):
   the TFER byte layout tools/entropy_capture_report.py reads, overflow
   accounting, and the entropy-capture.cfg parser, which must keep the
   recorder to a plain host0: file name and a bounded buffer. */
#include "../src/psp_entropy_capture.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

static uint32_t get16(const uint8_t *at)
{
    return (uint32_t) at[0] | ((uint32_t) at[1] << 8);
}

static uint32_t get32(const uint8_t *at)
{
    return get16(at) | (get16(at + 2) << 16);
}

static uint64_t get64(const uint8_t *at)
{
    return (uint64_t) get32(at) | ((uint64_t) get32(at + 4) << 32);
}

static bool parse(const char *text, PspEntropyCaptureConfig *config)
{
    char why[64] = "";
    return psp_entropy_capture_config_parse(text, strlen(text), config, why,
                                            sizeof(why));
}

static bool refused(const char *text, const char *reason)
{
    PspEntropyCaptureConfig config;
    memset(&config, 0x5a, sizeof(config));
    unsigned char untouched[sizeof(config)];
    memcpy(untouched, &config, sizeof(config));
    char why[64] = "";
    bool accepted = psp_entropy_capture_config_parse(
        text, strlen(text), &config, why, sizeof(why));
    if (accepted || strstr(why, reason) == NULL
        || memcmp(untouched, &config, sizeof(config)) != 0) {
        fprintf(stderr, "expected refusal \"%s\" for <%s>, got %d \"%s\"\n",
                reason, text, (int) accepted, why);
        return false;
    }
    return true;
}

static int test_layout(void)
{
    uint8_t bytes[40 + 40 + 3 * 4 + 40 + 2 * 4];
    PspEntropyCaptureLog log;
    CHECK(!psp_entropy_capture_log_init(&log, bytes, 39));
    CHECK(psp_entropy_capture_log_init(&log, bytes, sizeof(bytes)));
    const uint32_t work[3] = {7u, 8u, 0x89abcdefu};
    PspEntropyCaptureBatch first = {
        .source = PSP_ENTROPY_CAPTURE_SOURCE_WORK,
        .phase = PSP_ENTROPY_CAPTURE_PHASE_FILL,
        .count = 3, .millibits = 412, .credited_bits = 21,
        .pool_bits = 42, .batch_us = 1800,
        .start_us = UINT64_C(0x0102030405060708)
    };
    CHECK(psp_entropy_capture_log_append(&log, &first, work));
    /* Too big for what is left: dropped, counted, not written. */
    const uint32_t sleep[3] = {230u, 231u, 229u};
    PspEntropyCaptureBatch big = {
        .source = PSP_ENTROPY_CAPTURE_SOURCE_SLEEP,
        .phase = PSP_ENTROPY_CAPTURE_PHASE_EXTRA, .count = 3
    };
    CHECK(!psp_entropy_capture_log_append(&log, &big, sleep));
    /* An unknown source is refused the same way. */
    PspEntropyCaptureBatch unknown = { .source = 3, .count = 1 };
    CHECK(!psp_entropy_capture_log_append(&log, &unknown, sleep));
    PspEntropyCaptureBatch fits = {
        .source = PSP_ENTROPY_CAPTURE_SOURCE_SLEEP,
        .phase = PSP_ENTROPY_CAPTURE_PHASE_EXTRA, .count = 2,
        .millibits = PSP_ENTROPY_CAPTURE_NOT_ESTIMATED,
        .credited_bits = PSP_ENTROPY_CAPTURE_NOT_ESTIMATED,
        .pool_bits = 42, .batch_us = 470, .start_us = 99
    };
    CHECK(psp_entropy_capture_log_append(&log, &fits, sleep));
    CHECK(log.records == 2 && log.dropped == 2);
    CHECK(log.samples[0] == 3 && log.samples[1] == 2);
    PspEntropyCaptureShape shape = {256, 64, 32, 1};
    size_t length = psp_entropy_capture_log_finish(&log, &shape);
    CHECK(length == sizeof(bytes));

    CHECK(memcmp(bytes, "TFER", 4) == 0);
    CHECK(get16(bytes + 4) == 1 && get16(bytes + 6) == 40
          && get16(bytes + 8) == 40);
    CHECK(get16(bytes + 10) == PSP_ENTROPY_CAPTURE_FLAG_DROPPED);
    CHECK(get32(bytes + 12) == 2 && get32(bytes + 16) == 2);
    CHECK(get32(bytes + 20) == 256 && get32(bytes + 24) == 64
          && get32(bytes + 28) == 32 && get32(bytes + 32) == 1);
    CHECK(get32(bytes + 36) == 1000000u);

    const uint8_t *record = bytes + 40;
    CHECK(get16(record) == 1 && get16(record + 2) == 1);
    CHECK(get32(record + 4) == 0 && get32(record + 8) == 3);
    CHECK(get32(record + 12) == 412 && get32(record + 16) == 21);
    CHECK(get32(record + 20) == 42 && get32(record + 24) == 1800);
    CHECK(get32(record + 28) == 0);
    CHECK(get64(record + 32) == UINT64_C(0x0102030405060708));
    CHECK(get32(record + 40) == 7 && get32(record + 44) == 8
          && get32(record + 48) == 0x89abcdefu);
    /* The index counts the dropped records, so a gap shows. */
    record += 40 + 3 * 4;
    CHECK(get16(record) == 2 && get16(record + 2) == 3);
    CHECK(get32(record + 4) == 3 && get32(record + 8) == 2);
    CHECK(get32(record + 12) == UINT32_MAX && get32(record + 16) == UINT32_MAX);
    CHECK(get64(record + 32) == 99);
    CHECK(get32(record + 40) == 230 && get32(record + 44) == 231);

    /* Nothing dropped: no flag. */
    CHECK(psp_entropy_capture_log_init(&log, bytes, sizeof(bytes)));
    CHECK(psp_entropy_capture_log_append(&log, &first, work));
    (void) psp_entropy_capture_log_finish(&log, &shape);
    CHECK(get16(bytes + 10) == 0 && get32(bytes + 16) == 0);
    return 0;
}

static int test_config(void)
{
    PspEntropyCaptureConfig config;
    CHECK(parse("# entropy capture\r\noutput=entropy-1.tfer\r\n", &config));
    CHECK(strcmp(config.output, "entropy-1.tfer") == 0);
    CHECK(config.extra_rounds == 16 && config.work_run == 4096
          && config.sleep_run == 1024 && config.buffer_kib == 1024);
    CHECK(psp_entropy_capture_config_bytes(&config) <= 1024u * 1024u);
    CHECK(parse("extra_rounds=0\noutput=a\nwork_run=256\nsleep_run=64\n"
                "buffer_kib=256", &config));
    CHECK(config.extra_rounds == 0 && config.work_run == 256
          && config.sleep_run == 64 && config.buffer_kib == 256);
    CHECK(parse("output=x.tfer\nextra_rounds=64\nwork_run=16384\n"
                "sleep_run=4096\nbuffer_kib=8192\n", &config));

    /* A plain name beside the program on host0:, nothing else. */
    CHECK(refused("output=ms0:/x.tfer\n", "value out of range"));
    CHECK(refused("output=../x\n", "value out of range"));
    CHECK(refused("output=dir/x\n", "value out of range"));
    CHECK(refused("output=.hidden\n", "value out of range"));
    CHECK(refused("output=\n", "value out of range"));
    CHECK(refused("output=a b\n", "value out of range"));
    CHECK(refused("output=0123456789012345678901234567890123456789"
                  "01234567890123456789012345\n", "value out of range"));
    CHECK(refused("extra_rounds=4\n", "no output"));
    /* Bounds on every number. */
    CHECK(refused("output=a\nextra_rounds=65\n", "value out of range"));
    CHECK(refused("output=a\nwork_run=255\n", "value out of range"));
    CHECK(refused("output=a\nwork_run=16385\n", "value out of range"));
    CHECK(refused("output=a\nsleep_run=63\n", "value out of range"));
    CHECK(refused("output=a\nsleep_run=4097\n", "value out of range"));
    CHECK(refused("output=a\nbuffer_kib=63\n", "value out of range"));
    CHECK(refused("output=a\nbuffer_kib=8193\n", "value out of range"));
    CHECK(refused("output=a\nextra_rounds=-1\n", "value out of range"));
    CHECK(refused("output=a\nextra_rounds=4x\n", "value out of range"));
    CHECK(refused("output=a\nextra_rounds=\n", "value out of range"));
    /* The extra runs must fit the buffer they are given. */
    CHECK(refused("output=a\nextra_rounds=64\nwork_run=16384\n"
                  "sleep_run=4096\nbuffer_kib=4096\n", "buffer_kib too small"));
    CHECK(refused("output=a\nbuffer_kib=256\n", "buffer_kib too small"));
    /* Every key at most once, and nothing else. */
    CHECK(refused("output=a\noutput=b\n", "key repeated"));
    CHECK(refused("output=a\nseed=1\n", "unknown key"));
    CHECK(refused("output=a\n output=b\n", "unknown key"));
    CHECK(refused("output=a\nextra_rounds\n", "line without ="));
    static const char nul_text[] = "output=a\0b\n";
    char why[64] = "";
    CHECK(!psp_entropy_capture_config_parse(nul_text, sizeof(nul_text) - 1u,
                                            &config, why, sizeof(why))
          && strstr(why, "NUL") != NULL);
    return 0;
}

int main(void)
{
    if (test_layout() != 0 || test_config() != 0) return 1;
    puts("psp entropy capture tests passed");
    return 0;
}
