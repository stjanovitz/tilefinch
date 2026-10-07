#ifndef TILEFINCH_DECLARATIVE_REFRESH_H
#define TILEFINCH_DECLARATIVE_REFRESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The parsing half of the HTML Standard's "shared declarative refresh
   steps", used by <meta http-equiv="refresh" content> and by the `Refresh`
   response header alike:

     [ws] digits [digits or '.']... [ws] [';' or ','] [ws]
     [ "url" [ws] "=" [ws] ] [quote] url [quote]

   The time is whole seconds; any fractional part is ignored (".5" is 0).
   A missing URL means "refresh this document". Parsing is pure: the caller
   resolves the URL against the document (navigation does, see
   docs/ARCHITECTURE.md, "Declarative refresh") and decides whether the
   refresh may run. */
typedef struct {
    /* Saturates at UINT32_MAX for absurd inputs. */
    uint32_t seconds;
    /* False: no URL text followed the time (reload the document). */
    bool has_url;
    /* The URL text, before URL parsing: a span of the input. The URL
       parser's own rules (stripping leading/trailing C0 controls and
       spaces, removing tabs and newlines) are the resolver's job. */
    size_t url_offset;
    size_t url_length;
} DeclarativeRefresh;

/* False when the steps "return" (the input does not declare a refresh);
   true with *refresh filled otherwise. */
bool declarative_refresh_parse(const char *input, size_t length,
                               DeclarativeRefresh *refresh);

#endif
