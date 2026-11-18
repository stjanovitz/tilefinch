# Page-initiated navigations

This page is for engineers changing navigations a page starts without a user
press: declarative refresh, and the history handling of navigations page
script requests. The rules are summarized in
[Architecture](../ARCHITECTURE.md#declarative-refresh); this page holds the
mechanism. The request policy every such navigation passes is in the
[security model](../SECURITY_MODEL.md).

## Declarative refresh

`<meta http-equiv="refresh">` and the `Refresh` response header follow the
HTML Standard's shared declarative refresh steps, with or without JavaScript
(`src/declarative_refresh.c` parses; `src/navigation/declarative_refresh.inc`
owns the rest).

### Which refresh counts

Only a document's first refresh counts: the header, adopted when the document
is created, wins; otherwise the first connected HTML `<meta>` in tree order
whose content parses, in the head or the body, never inside `<template>`, and
inside `<noscript>` only when the page was parsed with scripting off. A meta
that page script inserts or changes later is noticed through the mutation
bridge and counts from that moment. The URL resolves against the document
base; no URL means the document itself.

### Timer and handoff

The timer is page state on the page clock: it starts on the first runtime
advance after commit (after the load event) and pauses whenever page work does
(menus, the keyboard, media). A due refresh is followed exactly like a
page-script navigation. Non-deferring owners such as the lab load it inside
the runtime advance; the PSP app, which defers page navigations
(`browser_engine_set_defer_script_navigation`), takes it with
`browser_engine_take_refresh_navigation` after any pending script navigation
(which wins) as an ordinary cancellable navigation job. The document is the
referrer, there is no user activation, the request passes the ordinary
top-level request policy, and the load replaces the current history entry.

### Cancellation

Any other navigation of the page (even a failed one), unloading it, or the
user editing a form field (`browser_engine_insert_text`,
`browser_engine_replace_text`, `browser_engine_backspace`) cancels a pending
refresh. Non-HTTP(S) targets are never followed but still are the document's
refresh, and a refresh in a child frame does nothing, so no frame can move the
top-level page.

### Keeping the page with the user

- `NAVIGATION_REFRESH_CHAIN_LIMIT` (5) bounds unattended refreshes: the sixth
  consecutive refresh navigation with no user input is stopped, the page
  stays, and the PSP status line says `AUTO-REFRESH STOPPED`. A button press,
  a typed or bookmarked address, or a user-activated link resets the count.
- A refresh to another URL is a redirect and is followed at any delay, but a
  same-URL refresh (an auto-reload or a fragment-only target) is followed only
  while the page is untouched. After any button press since load
  (`browser_engine_note_user_input`), it becomes `NAVIGATION_REFRESH_OFFERED`:
  `browser_engine_take_refresh_navigation` reports it once, and the PSP shows
  the non-modal note `PAGE WANTS TO RELOAD  SQUARE RELOADS`
  (`PSP_UI_STATUS_RELOAD_OFFER`), Square being the page's reload action. A
  reload would otherwise discard the page someone is reading at the cost of a
  full PSP load, while an unattended page still refreshes.

## Script navigations and history

A top-level navigation requested by page script keeps or replaces the current
history entry as the HTML Standard's history handling says.

- `location.replace()` replaces it: the loaded document takes the entry, so
  back skips the page that redirected (the same mechanism as a followed
  refresh, `navigation_arm_history_replace`). The arming belongs to the next
  navigation that starts, whichever it is, and only that navigation's load or
  direct commit (a site adapter's page) uses it.
- `location.assign()`, assigning `location.href` or a URL part, and following
  a link push a new entry.
- `location.reload()` and a fragment-only `replace()` keep the entry they are
  on, and `history.replaceState()` renames the current entry in place without
  loading anything.
