#ifndef TILEFINCH_LAYOUT_VALIDATION_DIFF_H
#define TILEFINCH_LAYOUT_VALIDATION_DIFF_H

/* Validation-only measurement for the layout subtree reuse study: after each
   complete build, how much of the new layout equals the published one (whole
   subtrees equal up to a translation), why the build ran, and (host only)
   how much flow time went to the subtrees that came out unchanged. Nothing
   here is compiled into shipping PSP builds. */

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define TILEFINCH_LAYOUT_DIFF 1

#include "tilefinch/layout.h"

/* Why the next completed build runs; a static string, read once by the
   report and reset to "other". */
/* Whether the study reports at all: always in PSP validation builds, and
   on the host only with the TILEFINCH_LAYOUT_DIFF switch set. Study log
   lines outside this file print only when it holds. */
bool layout_validation_enabled(void);
void layout_validation_set_cause(const char *cause);
/* The published layout later builds are compared against. Host builds also
   keep a private copy, so a build that replaced it in place still compares. */
void layout_validation_set_reference(const LayoutDocument *layout);
/* Called wherever a layout's boxes are freed or change owner (layout_destroy,
   layout_keep_image_boxes) so a reference to them is never read again. */
void layout_validation_forget(const LayoutDocument *layout);
/* One tilefinch-layout-diff line for a completed build. */
void layout_validation_report_build(const LayoutDocument *fresh);
/* Every box (geometry, client, content and scroll extents) and every draw
   command of two layouts of one document, in order; `detail` names the
   first difference. For TILEFINCH_VERIFY_GEOMETRY_PROOF. */
typedef enum {
    LAYOUT_VALIDATION_SAME,
    LAYOUT_VALIDATION_COMMANDS_DIFFER,
    LAYOUT_VALIDATION_BOXES_DIFFER
} LayoutValidationCompare;
LayoutValidationCompare layout_validation_compare(
    const LayoutDocument *expected, const LayoutDocument *actual,
    char *detail, size_t detail_size);
/* Free text appended to the next report line (adoption reasons). */
void layout_validation_note(const char *note);

#ifndef __PSP__
/* Flow time attribution, host only: time between consecutive cooperative
   checkpoints is charged to the node of the earlier one. */
void layout_validation_time_begin(const LayoutDocument *owner);
void layout_validation_time_node(const LayoutDocument *owner,
                                 const void *node);
#endif
#endif

#endif
