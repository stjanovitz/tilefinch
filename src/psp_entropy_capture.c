#include "psp_entropy_capture.h"

#include <stdio.h>
#include <string.h>

/* The pure half of the validation entropy capture (psp_entropy_capture.h):
   the TFER encoder and the entropy-capture.cfg parser. */

static void capture_put16(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t) value;
    at[1] = (uint8_t) (value >> 8);
}

static void capture_put32(uint8_t *at, uint32_t value)
{
    for (unsigned byte = 0; byte < 4u; byte++)
        at[byte] = (uint8_t) (value >> (8u * byte));
}

static void capture_put64(uint8_t *at, uint64_t value)
{
    for (unsigned byte = 0; byte < 8u; byte++)
        at[byte] = (uint8_t) (value >> (8u * byte));
}

bool psp_entropy_capture_log_init(PspEntropyCaptureLog *log, uint8_t *bytes,
                                  size_t capacity)
{
    if (log == NULL) return false;
    memset(log, 0, sizeof(*log));
    if (bytes == NULL || capacity < PSP_ENTROPY_CAPTURE_HEADER_BYTES)
        return false;
    log->bytes = bytes;
    log->capacity = capacity;
    log->length = PSP_ENTROPY_CAPTURE_HEADER_BYTES;
    memset(bytes, 0, PSP_ENTROPY_CAPTURE_HEADER_BYTES);
    return true;
}

bool psp_entropy_capture_log_append(PspEntropyCaptureLog *log,
                                    const PspEntropyCaptureBatch *batch,
                                    const uint32_t *samples)
{
    if (log == NULL || log->bytes == NULL || batch == NULL) return false;
    uint32_t index = log->records + log->dropped;
    size_t room = log->capacity - log->length;
    bool source_known = batch->source == PSP_ENTROPY_CAPTURE_SOURCE_WORK
        || batch->source == PSP_ENTROPY_CAPTURE_SOURCE_SLEEP;
    if (!source_known || (batch->count != 0 && samples == NULL)
        || room < PSP_ENTROPY_CAPTURE_RECORD_BYTES
        || (room - PSP_ENTROPY_CAPTURE_RECORD_BYTES) / 4u < batch->count) {
        log->dropped++;
        return false;
    }
    uint8_t *at = log->bytes + log->length;
    capture_put16(at + 0, batch->source);
    capture_put16(at + 2, batch->phase);
    capture_put32(at + 4, index);
    capture_put32(at + 8, batch->count);
    capture_put32(at + 12, batch->millibits);
    capture_put32(at + 16, batch->credited_bits);
    capture_put32(at + 20, batch->pool_bits);
    capture_put32(at + 24, batch->batch_us);
    capture_put32(at + 28, 0u);
    capture_put64(at + 32, batch->start_us);
    at += PSP_ENTROPY_CAPTURE_RECORD_BYTES;
    for (uint32_t sample = 0; sample < batch->count; sample++)
        capture_put32(at + 4u * sample, samples[sample]);
    log->length += PSP_ENTROPY_CAPTURE_RECORD_BYTES + 4u * (size_t) batch->count;
    log->records++;
    log->samples[batch->source - 1u] += batch->count;
    return true;
}

size_t psp_entropy_capture_log_finish(PspEntropyCaptureLog *log,
                                      const PspEntropyCaptureShape *shape)
{
    if (log == NULL || log->bytes == NULL) return 0;
    uint8_t *at = log->bytes;
    memcpy(at, "TFER", 4);
    capture_put16(at + 4, PSP_ENTROPY_CAPTURE_VERSION);
    capture_put16(at + 6, PSP_ENTROPY_CAPTURE_HEADER_BYTES);
    capture_put16(at + 8, PSP_ENTROPY_CAPTURE_RECORD_BYTES);
    capture_put16(at + 10, log->dropped != 0
                               ? PSP_ENTROPY_CAPTURE_FLAG_DROPPED : 0u);
    capture_put32(at + 12, log->records);
    capture_put32(at + 16, log->dropped);
    capture_put32(at + 20, shape != NULL ? shape->work_batch : 0u);
    capture_put32(at + 24, shape != NULL ? shape->sleep_batch : 0u);
    capture_put32(at + 28, shape != NULL ? shape->work_touches : 0u);
    capture_put32(at + 32, shape != NULL ? shape->sleep_us : 0u);
    capture_put32(at + 36, 1000000u);
    return log->length;
}

uint64_t psp_entropy_capture_config_bytes(
    const PspEntropyCaptureConfig *config)
{
    uint64_t run = config->work_run > config->sleep_run
        ? config->work_run : config->sleep_run;
    uint64_t round = 2u * PSP_ENTROPY_CAPTURE_RECORD_BYTES
        + 4u * ((uint64_t) config->work_run + config->sleep_run);
    return PSP_ENTROPY_CAPTURE_HEADER_BYTES
        + PSP_ENTROPY_CAPTURE_BOOT_RESERVE
        + (uint64_t) config->extra_rounds * round + 4u * run;
}

static bool capture_refuse(char *why, size_t why_size, const char *reason)
{
    if (why != NULL && why_size != 0) snprintf(why, why_size, "%s", reason);
    return false;
}

static bool capture_number(const char *value, size_t length,
                           uint32_t minimum, uint32_t maximum,
                           uint32_t *out)
{
    if (length == 0 || length > 6u) return false;
    uint32_t number = 0;
    for (size_t at = 0; at < length; at++) {
        if (value[at] < '0' || value[at] > '9') return false;
        number = number * 10u + (uint32_t) (value[at] - '0');
    }
    if (number < minimum || number > maximum) return false;
    *out = number;
    return true;
}

static bool capture_output_valid(const char *value, size_t length)
{
    if (length == 0 || length > PSP_ENTROPY_CAPTURE_OUTPUT_LIMIT
        || value[0] == '.')
        return false;
    for (size_t at = 0; at < length; at++) {
        unsigned character = (unsigned char) value[at];
        if (!((character >= 'a' && character <= 'z')
              || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9')
              || character == '-' || character == '_' || character == '.'))
            return false;
    }
    return true;
}

bool psp_entropy_capture_config_parse(const char *text, size_t length,
                                      PspEntropyCaptureConfig *config,
                                      char *why, size_t why_size)
{
    if (text == NULL || config == NULL)
        return capture_refuse(why, why_size, "no text");
    PspEntropyCaptureConfig parsed = {
        .extra_rounds = 16u, .work_run = 4096u, .sleep_run = 1024u,
        .buffer_kib = 1024u
    };
    static const char *const keys[] = {
        "output", "extra_rounds", "work_run", "sleep_run", "buffer_kib"
    };
    bool seen[sizeof(keys) / sizeof(keys[0])] = {false};
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') end++;
        size_t line_length = end - at;
        const char *line = text + at;
        at = end + (end < length ? 1u : 0u);
        if (line_length != 0 && line[line_length - 1u] == '\r') line_length--;
        if (memchr(line, '\0', line_length) != NULL)
            return capture_refuse(why, why_size, "NUL in config");
        if (line_length == 0 || line[0] == '#') continue;
        const char *equals = memchr(line, '=', line_length);
        if (equals == NULL)
            return capture_refuse(why, why_size, "line without =");
        size_t key_length = (size_t) (equals - line);
        const char *value = equals + 1;
        size_t value_length = line_length - key_length - 1u;
        size_t key = 0;
        while (key < sizeof(keys) / sizeof(keys[0])
               && !(strlen(keys[key]) == key_length
                    && memcmp(keys[key], line, key_length) == 0))
            key++;
        if (key == sizeof(keys) / sizeof(keys[0]))
            return capture_refuse(why, why_size, "unknown key");
        if (seen[key]) return capture_refuse(why, why_size, "key repeated");
        seen[key] = true;
        bool valid;
        switch (key) {
        case 0:
            valid = capture_output_valid(value, value_length);
            if (valid) {
                memcpy(parsed.output, value, value_length);
                parsed.output[value_length] = '\0';
            }
            break;
        case 1:
            valid = capture_number(value, value_length, 0u, 64u,
                                   &parsed.extra_rounds);
            break;
        case 2:
            valid = capture_number(value, value_length, 256u, 16384u,
                                   &parsed.work_run);
            break;
        case 3:
            valid = capture_number(value, value_length, 64u, 4096u,
                                   &parsed.sleep_run);
            break;
        default:
            valid = capture_number(value, value_length, 64u, 8192u,
                                   &parsed.buffer_kib);
            break;
        }
        if (!valid) return capture_refuse(why, why_size, "value out of range");
    }
    if (!seen[0]) return capture_refuse(why, why_size, "no output");
    if (psp_entropy_capture_config_bytes(&parsed)
        > (uint64_t) parsed.buffer_kib * 1024u)
        return capture_refuse(why, why_size, "buffer_kib too small");
    *config = parsed;
    return true;
}
