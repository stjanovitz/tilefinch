#ifndef TILEFINCH_SELECTOR_SYNTAX_H
#define TILEFINCH_SELECTOR_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>

/* Whether `text` (UTF-8, `length` bytes) parses as a selector list by the
   Selectors Level 4 grammar, as DOM's "parse a selector" requires before
   querySelector(), matches() and closest() may run. Valid syntax that the
   native matcher does not implement is refused too (namespace prefixes,
   comments, the column combinator, invalid entries inside :is() and
   :where()), as the Selectors standard asks of a UA for unsupported
   features. Linear in `length`; nesting of functional pseudo-classes is
   capped at SELECTOR_SYNTAX_NESTING_LIMIT. */
#define SELECTOR_SYNTAX_NESTING_LIMIT 16u

bool selector_syntax_valid(const char *text, size_t length);

#endif
