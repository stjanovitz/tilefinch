#ifndef TILEFINCH_MEDIA_CHAPTERS_H
#define TILEFINCH_MEDIA_CHAPTERS_H

#include <stdint.h>

/* Small, immutable metadata beside a player, never in a copied stream record. */
#define MEDIA_CHAPTER_LIMIT 24u
#define MEDIA_CHAPTER_TITLE_CAPACITY 80u
typedef struct {
    uint32_t seconds;
    char title[MEDIA_CHAPTER_TITLE_CAPACITY];
} MediaChapter;

#endif
