#ifndef TILEFINCH_PSP_READER_POLICY_H
#define TILEFINCH_PSP_READER_POLICY_H

#include <stdbool.h>

#include "tilefinch/psp_ui.h"
#include "tilefinch/reader_mode.h"

/*
 * Who engaged the PSP frontend's Reader view, and what follows from it
 * (docs/READER_MODE.md). Reader the user turned on is sticky: a followed
 * link opens in Reader too. Reader that Auto Reader engaged belongs to that
 * one page: the next navigation leaves it, and the destination gets its own
 * confidence check. Kept free of engine and firmware state so it is a host
 * test.
 */

/* The note Auto Reader shows when it switches a page to Reader. Circle is
   Back in Reader as everywhere on a page, so the note names the menu path
   that shows the full page instead. */
#define PSP_READER_AUTOMATIC_NOTE \
    "Reader view (Auto Reader)\nSelect, Page tools, Reader mode: full page"
#define PSP_READER_AUTOMATIC_NOTE_FRAMES 240u

/* Auto Reader engages only on a high-confidence, non-raw analysis. */
bool psp_reader_automatic_admits(const ReaderDocumentAnalysis *analysis);

/* Record that Reader is now presented. An automatic engagement shows
   PSP_READER_AUTOMATIC_NOTE, a short status that clears by itself. */
void psp_reader_mark_engaged(PspUiState *ui, bool automatic);

/* Record that Reader is no longer presented. */
void psp_reader_mark_left(PspUiState *ui);

/* Whether a followed link should open in Reader: only Reader the user
   turned on carries over. */
bool psp_reader_carries_to_navigation(const PspUiState *ui);

#endif
