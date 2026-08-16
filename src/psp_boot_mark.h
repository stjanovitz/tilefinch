#ifndef TILEFINCH_PSP_BOOT_MARK_H
#define TILEFINCH_PSP_BOOT_MARK_H

/*
 * Canonical geometry for the in-app Tilefinch boot mark. The stable launcher
 * and the slot browser deliberately have separate renderers and binaries, but
 * both expand these primitive lists so the visible LoadExec handoff cannot
 * drift. This is not the packaged XMB ICON0 artwork.
 *
 * Each list passes its final argument through as a colour expression. Callers
 * provide local `accent`, `body`, and `eye` values, then define a short DRAW
 * macro appropriate to their renderer before expanding the list.
 */
#define TILEFINCH_BOOT_MARK_UNITS 48
#define TILEFINCH_BOOT_MARK_TOP 100

#define TILEFINCH_BOOT_MARK_ROUND_RECTS(DRAW) \
    DRAW(0, 0, 48, 48, 12, accent)            \
    DRAW(13, 15, 25, 18, 9, body)             \
    DRAW(26, 10, 14, 15, 7, body)

#define TILEFINCH_BOOT_MARK_TRIANGLES(DRAW) \
    DRAW(39, 15, 46, 19, 39, 23, body)       \
    DRAW(15, 22, 6, 16, 10, 28, body)        \
    DRAW(17, 24, 8, 32, 21, 31, body)        \
    DRAW(18, 21, 32, 25, 19, 31, accent)

#define TILEFINCH_BOOT_MARK_RECTS(DRAW) \
    DRAW(33, 14, 3, 3, eye)

#endif
