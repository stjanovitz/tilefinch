#ifndef TILEFINCH_BACKGROUND_CHUNK_H
#define TILEFINCH_BACKGROUND_CHUNK_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/*
 * A streaming slot's chunk is buffer[offset, offset + length). The browser
 * takes from the front by advancing offset instead of moving the rest of
 * the chunk down: an HLS source takes one 188-byte TS packet at a time from
 * chunks of up to 48 KiB, and moving the remainder after every take copied
 * each byte about chunk/376 times on the browser thread.
 *
 * Ownership is unchanged: whoever owns the chunk (the worker while it is
 * EMPTY, the browser while it holds TAKING) owns offset and length too.
 * A drained chunk returns offset to 0, so the worker's appends normally
 * start at the front; an append that would not fit after a non-zero offset
 * moves the chunk down first.
 */

/* The browser took `taken` bytes from the front, leaving `remaining`. */
static inline size_t fetch_background_chunk_advance(
    size_t offset, size_t taken, size_t remaining)
{
    return remaining != 0 ? offset + taken : 0;
}

/* Where an append of `count` bytes goes, or NULL when the chunk cannot hold
   them. Moves the chunk to the front of the buffer only when the space after
   it is too small. */
static inline unsigned char *fetch_background_chunk_reserve(
    unsigned char *buffer, size_t capacity, size_t *offset, size_t length,
    size_t count)
{
    if (buffer == NULL || offset == NULL || *offset > capacity
        || length > capacity - *offset || count > capacity - length)
        return NULL;
    if (count > capacity - *offset - length) {
        memmove(buffer, buffer + *offset, length);
        *offset = 0;
    }
    return buffer + *offset + length;
}

#endif
