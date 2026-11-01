#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/fetch/background_chunk.h"

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "background-chunk failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #value); failures++; \
} } while (0)

#define CAPACITY (64u * 1024u)
#define TS_PACKET 188u

/* One streaming slot as background_transport.inc drives it: the worker
   appends only while the chunk is EMPTY and publishes it at its target;
   the browser takes from the front while it holds it. */
typedef struct {
    unsigned char buffer[CAPACITY];
    size_t offset;
    size_t length;
    bool ready;
} ModelChunk;

static bool model_append(ModelChunk *chunk, const unsigned char *data,
                         size_t count)
{
    if (chunk->ready) return false;
    unsigned char *append = fetch_background_chunk_reserve(
        chunk->buffer, CAPACITY, &chunk->offset, chunk->length, count);
    if (append == NULL) return false;
    memcpy(append, data, count);
    chunk->length += count;
    return true;
}

/* The same steps as fetch_background_transport_take_chunk. */
static size_t model_take(ModelChunk *chunk, unsigned char *destination,
                         size_t capacity)
{
    if (!chunk->ready || chunk->length == 0 || capacity == 0) return 0;
    size_t available = chunk->length;
    size_t taken = available < capacity ? available : capacity;
    memcpy(destination, chunk->buffer + chunk->offset, taken);
    size_t remaining = available - taken;
    chunk->offset = fetch_background_chunk_advance(
        chunk->offset, taken, remaining);
    chunk->length = remaining;
    if (remaining == 0) chunk->ready = false;
    return taken;
}

static unsigned char pattern(size_t at)
{
    return (unsigned char) ((at * 131u + (at >> 8) * 7u) & 0xffu);
}

static void fill(unsigned char *data, size_t start, size_t count)
{
    for (size_t i = 0; i < count; i++) data[i] = pattern(start + i);
}

static void test_many_small_takes_leave_the_rest_in_place(void)
{
    static ModelChunk chunk;
    memset(&chunk, 0, sizeof(chunk));
    static unsigned char data[48u * 1024u];
    fill(data, 0, sizeof(data));
    CHECK(model_append(&chunk, data, sizeof(data)));
    chunk.ready = true;
    const unsigned char *rest = chunk.buffer + TS_PACKET;
    size_t delivered = 0;
    unsigned char packet[TS_PACKET];
    bool in_order = true;
    bool stayed = true;
    while (chunk.ready) {
        size_t before = chunk.offset;
        size_t taken = model_take(&chunk, packet, sizeof(packet));
        for (size_t i = 0; i < taken; i++)
            if (packet[i] != pattern(delivered + i)) in_order = false;
        delivered += taken;
        if (chunk.ready) {
            /* The rest was not moved: the next byte is where it was. */
            if (chunk.offset != before + taken
                || chunk.buffer + chunk.offset != rest + before) stayed = false;
        }
    }
    CHECK(in_order);
    CHECK(stayed);
    CHECK(delivered == sizeof(data));
    CHECK(chunk.offset == 0 && chunk.length == 0);
    /* 48 KiB is not a whole number of TS packets: the last take is short. */
    CHECK(sizeof(data) % TS_PACKET != 0);
}

static void test_take_at_chunk_end(void)
{
    static ModelChunk chunk;
    memset(&chunk, 0, sizeof(chunk));
    unsigned char data[1000];
    fill(data, 0, sizeof(data));
    CHECK(model_append(&chunk, data, sizeof(data)));
    chunk.ready = true;
    unsigned char out[1000];
    CHECK(model_take(&chunk, out, 600) == 600);
    CHECK(chunk.offset == 600 && chunk.length == 400 && chunk.ready);
    /* Exactly the rest: the chunk drains and its start returns to 0. */
    CHECK(model_take(&chunk, out + 600, 400) == 400);
    CHECK(chunk.offset == 0 && chunk.length == 0 && !chunk.ready);
    CHECK(memcmp(out, data, sizeof(data)) == 0);
    CHECK(model_take(&chunk, out, 1) == 0);

    /* More room than the rest: the take is the rest. */
    CHECK(model_append(&chunk, data, 300));
    chunk.ready = true;
    CHECK(model_take(&chunk, out, 100) == 100);
    CHECK(model_take(&chunk, out, 4096) == 200);
    CHECK(chunk.offset == 0 && chunk.length == 0 && !chunk.ready);
    CHECK(memcmp(out, data + 100, 200) == 0);
}

static void test_reserve_moves_the_chunk_only_when_needed(void)
{
    static unsigned char buffer[CAPACITY];
    for (size_t i = 0; i < CAPACITY; i++) buffer[i] = pattern(i);
    size_t offset = 1000;
    size_t length = 500;
    /* Exactly the space after the chunk: appended in place. */
    size_t after = CAPACITY - offset - length;
    unsigned char *at = fetch_background_chunk_reserve(
        buffer, CAPACITY, &offset, length, after);
    CHECK(at == buffer + 1500 && offset == 1000);
    /* One byte more: the chunk moves to the front, its bytes intact. */
    at = fetch_background_chunk_reserve(
        buffer, CAPACITY, &offset, length, after + 1);
    CHECK(at == buffer + 500 && offset == 0);
    bool intact = true;
    for (size_t i = 0; i < 500; i++)
        if (buffer[i] != pattern(1000 + i)) intact = false;
    CHECK(intact);
    /* The whole free space after moving fits; one byte beyond does not. */
    offset = 1000;
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, &offset, length, CAPACITY - length)
          == buffer + length);
    CHECK(offset == 0);
    offset = 1000;
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, &offset, length, CAPACITY - length + 1)
          == NULL);
    CHECK(offset == 1000);
    /* A full chunk at the front takes nothing more; an empty one at the
       front takes the whole buffer. */
    offset = 0;
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, &offset, CAPACITY, 1) == NULL);
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, &offset, 0, CAPACITY) == buffer);
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, &offset, 0, 0) == buffer);
    /* Inconsistent cursors are refused rather than trusted. */
    offset = CAPACITY + 1u;
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, &offset, 0, 1) == NULL);
    offset = CAPACITY - 10u;
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, &offset, 11, 0) == NULL);
    CHECK(fetch_background_chunk_reserve(
              NULL, CAPACITY, &offset, 0, 1) == NULL);
    CHECK(fetch_background_chunk_reserve(
              buffer, CAPACITY, NULL, 0, 1) == NULL);
}

static void test_advance(void)
{
    CHECK(fetch_background_chunk_advance(0, 188, 1000) == 188);
    CHECK(fetch_background_chunk_advance(376, 188, 1) == 564);
    CHECK(fetch_background_chunk_advance(376, 188, 0) == 0);
    CHECK(fetch_background_chunk_advance(0, 0, 0) == 0);
}

/* A whole stream: curl callbacks of varying size append while the chunk is
   EMPTY and publish it at the target or at the end; the browser takes
   packets, and sometimes larger slices, between callbacks. A worker append
   after the browser has partly drained a chunk is modelled too (the
   compaction path), although the transport only appends while EMPTY. */
static void test_interleaved_stream(bool append_into_partial)
{
    static ModelChunk chunk;
    memset(&chunk, 0, sizeof(chunk));
    static unsigned char wire[600u * 1024u];
    fill(wire, 0, sizeof(wire));
    static unsigned char received[sizeof(wire)];
    size_t sent = 0, got = 0;
    uint32_t rng = append_into_partial ? 7u : 3u;
    const size_t target = 16u * 1024u;
    unsigned compactions = 0;
    unsigned guard = 0;
    while (got < sizeof(wire) && guard++ < 1000000u) {
        rng = rng * 1103515245u + 12345u;
        unsigned choice = (rng >> 16) % 4u;
        if (choice == 0 && sent < sizeof(wire)) {
            size_t count = 1u + (rng >> 4) % (16u * 1024u);
            if (count > sizeof(wire) - sent) count = sizeof(wire) - sent;
            if (append_into_partial && chunk.ready) chunk.ready = false;
            size_t offset_before = chunk.offset;
            if (model_append(&chunk, wire + sent, count)) {
                if (offset_before != 0 && chunk.offset == 0) compactions++;
                sent += count;
            }
            if (chunk.length >= target || sent == sizeof(wire))
                chunk.ready = chunk.length != 0;
        } else {
            if (!chunk.ready && chunk.length != 0
                && (chunk.length >= target || sent == sizeof(wire)))
                chunk.ready = true;
            size_t capacity = choice == 3 ? 1u + (rng >> 6) % 9000u
                                          : TS_PACKET;
            got += model_take(&chunk, received + got, capacity);
        }
    }
    CHECK(got == sizeof(wire));
    CHECK(memcmp(received, wire, sizeof(wire)) == 0);
    CHECK(chunk.offset == 0 && chunk.length == 0);
    if (append_into_partial) CHECK(compactions != 0);
}

int main(void)
{
    test_many_small_takes_leave_the_rest_in_place();
    test_take_at_chunk_end();
    test_reserve_moves_the_chunk_only_when_needed();
    test_advance();
    test_interleaved_stream(false);
    test_interleaved_stream(true);
    if (failures != 0) return 1;
    puts("background-chunk tests passed");
    return 0;
}
