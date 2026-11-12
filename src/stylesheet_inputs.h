#ifndef TILEFINCH_STYLESHEET_INPUTS_H
#define TILEFINCH_STYLESHEET_INPUTS_H

/* The element attributes, besides stylesheet text, that decide what the
   stylesheet loader (src/resources.c) applies: those of a
   <link rel=stylesheet> and those of the root element. The loader reads
   each one by these names, and every compiled-sheet key and continuation
   check (src/navigation/stylesheet_checkpoint.inc) hashes every entry of
   the lists below. An attribute the loader starts to read goes here, or a
   cached or continued sheet stays stale when only that attribute changes.
   A link's nonce is read through document_element_nonce, as its CSP grant
   is, so it is hashed apart from these lists. */
#define STYLESHEET_INPUT_REL "rel"
#define STYLESHEET_INPUT_HREF "href"
#define STYLESHEET_INPUT_MEDIA "media"
#define STYLESHEET_INPUT_REFERRER_POLICY "referrerpolicy"
#define STYLESHEET_INPUT_INTEGRITY "integrity"
#define STYLESHEET_INPUT_CROSSORIGIN "crossorigin"
#define STYLESHEET_INPUT_DISABLED "disabled"
/* An inert theme-registry link (data-color-theme and data-href) and the
   root's selection among those themes. */
#define STYLESHEET_INPUT_THEME "data-color-theme"
#define STYLESHEET_INPUT_THEME_HREF "data-href"
#define STYLESHEET_INPUT_COLOR_MODE "data-color-mode"
#define STYLESHEET_INPUT_LIGHT_THEME "data-light-theme"
#define STYLESHEET_INPUT_DARK_THEME "data-dark-theme"

/* In key order. */
#define STYLESHEET_LINK_INPUTS(X) \
    X(STYLESHEET_INPUT_REL) X(STYLESHEET_INPUT_HREF) \
    X(STYLESHEET_INPUT_MEDIA) X(STYLESHEET_INPUT_REFERRER_POLICY) \
    X(STYLESHEET_INPUT_INTEGRITY) X(STYLESHEET_INPUT_CROSSORIGIN) \
    X(STYLESHEET_INPUT_DISABLED) X(STYLESHEET_INPUT_THEME) \
    X(STYLESHEET_INPUT_THEME_HREF)
#define STYLESHEET_ROOT_INPUTS(X) \
    X(STYLESHEET_INPUT_COLOR_MODE) X(STYLESHEET_INPUT_LIGHT_THEME) \
    X(STYLESHEET_INPUT_DARK_THEME)

#endif
