#ifndef TILEFINCH_PUBLIC_SUFFIX_H
#define TILEFINCH_PUBLIC_SUFFIX_H

#include <stdbool.h>
#include <stddef.h>

/* Classify a canonical ASCII DNS name with the bundled Public Suffix List.
   A false return means the input is not a classifiable DNS name; callers
   making a security decision must keep their conservative fallback. */
bool tilefinch_public_suffix_classify(const char *domain,
                                   bool *is_public_suffix);

/* Return the registrable domain (public suffix plus one label). A public
   suffix by itself, an IP literal, or invalid DNS input has no result. */
bool tilefinch_registrable_domain(const char *host, char *output,
                               size_t output_size);

#endif
