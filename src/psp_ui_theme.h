#ifndef TILEFINCH_PSP_UI_THEME_H
#define TILEFINCH_PSP_UI_THEME_H

/*
 * Chrome design tokens for the "Ember" visual language, flattened for the
 * RGB565 flat-fill renderer: every alpha-over-ground composite in the
 * original design is pre-multiplied here into a solid color. Chrome surfaces
 * draw with these tokens only; page content colors stay owned by the engine.
 *
 * The accent is theme-selectable (Options -> Theme); everything else is
 * fixed so the three accents stay legible on the same grounds.
 */

#include <stdint.h>
#include "tilefinch/pixel_math.h"

/* rgb888 -> RGB565 at compile time; keeps the tokens readable as hex. */
#define PSP_THEME_RGB(r, g, b) \
    TILEFINCH_RGB565_PACK_CODES_CONST((r) >> 3, (g) >> 2, (b) >> 3)

/* Grounds (dark, warm bias). */
#define PSP_THEME_GROUND         PSP_THEME_RGB(0x10, 0x0E, 0x0D)
#define PSP_THEME_GROUND_READER  PSP_THEME_RGB(0x14, 0x10, 0x0E)
#define PSP_THEME_CHROME_BAR     PSP_THEME_RGB(0x0E, 0x0C, 0x0B)
#define PSP_THEME_HINT_BAR       PSP_THEME_RGB(0x0A, 0x09, 0x08)
#define PSP_THEME_PANEL          PSP_THEME_RGB(0x14, 0x0F, 0x0D)

/* Raised surfaces (prototype's rgba(255,255,255,a) over the ground). */
#define PSP_THEME_SURFACE        PSP_THEME_RGB(0x1B, 0x19, 0x18) /* a=.045 */
#define PSP_THEME_SURFACE_FOCUS  PSP_THEME_RGB(0x26, 0x23, 0x22) /* a=.09  */
#define PSP_THEME_LINE           PSP_THEME_RGB(0x23, 0x21, 0x20) /* a=.08  */

/* Text. */
#define PSP_THEME_TEXT           PSP_THEME_RGB(0xFB, 0xF6, 0xF2)
#define PSP_THEME_TEXT_BODY      PSP_THEME_RGB(0xD2, 0xCC, 0xC8) /* a=.86 */
#define PSP_THEME_TEXT_MUTED     PSP_THEME_RGB(0x89, 0x84, 0x80) /* secondary */
#define PSP_THEME_TEXT_FAINT     PSP_THEME_RGB(0x68, 0x63, 0x60) /* disabled */

/* Accents. Ember is the default; the on-accent ink is shared. */
#define PSP_THEME_ACCENT_EMBER   PSP_THEME_RGB(0xE8, 0x82, 0x3C)
#define PSP_THEME_ACCENT_EMBER_HI PSP_THEME_RGB(0xF4, 0xA5, 0x6F)
#define PSP_THEME_ACCENT_COBALT  PSP_THEME_RGB(0x5A, 0xA2, 0xF0)
#define PSP_THEME_ACCENT_COBALT_HI PSP_THEME_RGB(0x8F, 0xC1, 0xF6)
#define PSP_THEME_ACCENT_SLATE   PSP_THEME_RGB(0x9F, 0xB0, 0xBF)
#define PSP_THEME_ACCENT_SLATE_HI PSP_THEME_RGB(0xC2, 0xCE, 0xD8)
#define PSP_THEME_ON_ACCENT      PSP_THEME_RGB(0x17, 0x12, 0x0E)

/* Semantic (not accents; identical across themes). */
#define PSP_THEME_OK             PSP_THEME_RGB(0x7F, 0xD6, 0xA2)
#define PSP_THEME_WARN           PSP_THEME_RGB(0xE0, 0x8A, 0x52)

/* Type scale (pixel sizes at 1x on the 480x272 panel).
   TITLE/BODY/LIST render with the proportional FreeType faces
   (TilefinchSans; DejaVu Serif in reader surfaces); LABEL stays on the
   compact caps face for small all-caps chrome labels and hints. */
#define PSP_THEME_TYPE_TITLE_LG  17
#define PSP_THEME_TYPE_TITLE     13
#define PSP_THEME_TYPE_BODY      11
#define PSP_THEME_TYPE_LIST      11
#define PSP_THEME_TYPE_LABEL      8

/* Spacing grid and radii (px). */
#define PSP_THEME_SPACE_XS        4
#define PSP_THEME_SPACE_S         6
#define PSP_THEME_SPACE_M         8
#define PSP_THEME_SPACE_L        12
#define PSP_THEME_SPACE_XL       16
#define PSP_THEME_RADIUS_CHIP     3
#define PSP_THEME_RADIUS_ROW      4
#define PSP_THEME_RADIUS_CARD     5
#define PSP_THEME_RADIUS_PANEL    6

/* Focus ring: 2px accent stroke outside the surface; pressed state fills
   the surface with the accent and inverts text to PSP_THEME_ON_ACCENT;
   disabled renders text at PSP_THEME_TEXT_FAINT with no surface. */
#define PSP_THEME_FOCUS_STROKE    2

/* Motion budget (frames at 60 Hz; all easing from small precomputed
   tables, no floating point in the frame path). Purposeful motion only:
   panel slide-in, toast entry, focus settle, progress. */
#define PSP_THEME_MOTION_PANEL_FRAMES  8
#define PSP_THEME_MOTION_TOAST_FRAMES  5
#define PSP_THEME_MOTION_FOCUS_FRAMES  3

/* Ambient wave (home surface only): a pre-rendered 320px-wide tileable
   band scrolled by source offset. Advances on a fixed-point accumulator
   at WAVE_FPS; pauses after WAVE_IDLE_S of no input and whenever the
   power policy has stepped the clock down. Off when the profile option
   is off. */
#define PSP_THEME_WAVE_FPS            20
#define PSP_THEME_WAVE_BAND_WIDTH    320
#define PSP_THEME_WAVE_IDLE_SECONDS   30

#endif
