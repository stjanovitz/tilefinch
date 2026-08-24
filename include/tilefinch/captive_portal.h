#ifndef TILEFINCH_CAPTIVE_PORTAL_H
#define TILEFINCH_CAPTIVE_PORTAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/url.h"

/* The probe is user-initiated and deliberately small. A successful Internet
   path returns 204 with no body; redirects and bounded HTML are evidence that
   the access network wants an interactive sign-in. */
#define TILEFINCH_CAPTIVE_PORTAL_PROBE_URL \
    "http://connectivitycheck.gstatic.com/generate_204"
#define TILEFINCH_CAPTIVE_PORTAL_PROBE_BODY_LIMIT (8u * 1024u)
#define TILEFINCH_CAPTIVE_PORTAL_ORIGIN_LIMIT 4u

typedef enum {
    TILEFINCH_CAPTIVE_PROBE_INTERNET = 0,
    TILEFINCH_CAPTIVE_PROBE_PORTAL,
    TILEFINCH_CAPTIVE_PROBE_RETRYABLE,
    TILEFINCH_CAPTIVE_PROBE_FAILED
} TilefinchCaptiveProbeResult;

typedef struct {
    long status_code;
    const char *content_type;
    const char *location;
    const unsigned char *body;
    size_t body_length;
    bool transport_succeeded;
    bool timed_out;
} TilefinchCaptiveProbeResponse;

/* Pure classification used by the PSP operation and deterministic host tests.
   The probe never needs to parse or execute the returned page. */
TilefinchCaptiveProbeResult tilefinch_captive_probe_classify(
    const TilefinchCaptiveProbeResponse *response);

/* Resolves the portal destination without accepting non-HTTP schemes. An
   empty Location selects the probe URL itself for intercepted 200/511 pages. */
bool tilefinch_captive_probe_portal_url(
    const char *probe_url, const char *location,
    char output[TILEFINCH_URL_SERIALIZED_LIMIT]);

#endif
