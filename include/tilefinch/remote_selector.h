#ifndef TILEFINCH_REMOTE_SELECTOR_H
#define TILEFINCH_REMOTE_SELECTOR_H

#include <stddef.h>

#define TILEFINCH_REMOTE_SELECTOR_MATCH_LIMIT 128
#define TILEFINCH_REMOTE_TRAVERSAL_NODE_LIMIT 128

typedef struct {
    size_t section_index;
    size_t source_offset;
    char tag_name[32];
    char identifier[129];
    char stable_key[96];
} TilefinchRemoteSelectorMatch;

typedef struct {
    size_t section_index;
    unsigned node_type;
    unsigned special;
    char tag_name[32];
    char stable_key[96];
} TilefinchRemoteTraversalNode;

#endif
