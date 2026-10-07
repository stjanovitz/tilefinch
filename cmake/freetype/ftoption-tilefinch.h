/* FreeType options for Tilefinch page fonts: the upstream defaults without
 * PostScript glyph names.
 *
 * Selected through the FT_CONFIG_OPTIONS_H compile definition set in
 * cmake/freetype_vendored.cmake, beside ftmodule-minimal.h. That module list
 * deliberately registers no `psnames' module. With glyph-name support still
 * compiled in, sfnt synthesizes a Unicode cmap for any TrueType font whose
 * `post' table names glyphs but which has no Unicode cmap, and that cmap's
 * init dereferences the absent psnames service: a page font could crash
 * FT_New_Memory_Face(). src/font.c never asks for glyph names.
 */

#include <freetype/config/ftoption.h>

#undef FT_CONFIG_OPTION_POSTSCRIPT_NAMES
#undef FT_CONFIG_OPTION_ADOBE_GLYPH_LIST

/* Code size (PSP .text). src/font.c opens face 0 of an in-memory font,
 * selects the Unicode cmap, and loads unhinted outlines with
 * FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_BITMAP |
 * FT_LOAD_NO_SVG. Everything below is reachable only through APIs it never
 * calls, so the outlines, advances and kerning it reads do not change:
 * - variation fonts: no design coordinates are ever set, so FreeType already
 *   loads the default instance (the plain `glyf' outlines);
 * - COLR/CPAL layers and OT-SVG documents: colour glyphs are never requested
 *   (no FT_LOAD_COLOR, and FT_LOAD_NO_SVG);
 * - glyph names from `post', `name'-table access (FT_Get_Sfnt_Name), BDF
 *   properties and the incremental-loading interface: never called;
 * - cmap formats 2 (legacy multi-byte encodings) and 14 (Unicode variation
 *   sequences): never selected as the Unicode charmap;
 * - Mac resource-fork and dfont containers: browsers do not accept them as
 *   web fonts; without the fallback such data fails to load like any other
 *   unknown format. */
#undef TT_CONFIG_OPTION_GX_VAR_SUPPORT
#undef TT_CONFIG_OPTION_COLOR_LAYERS
#undef TT_SUPPORT_COLRV1
#undef FT_CONFIG_OPTION_SVG
#undef TT_CONFIG_OPTION_POSTSCRIPT_NAMES
#undef TT_CONFIG_OPTION_SFNT_NAMES
#undef TT_CONFIG_OPTION_BDF
#undef FT_CONFIG_OPTION_INCREMENTAL
#undef TT_CONFIG_CMAP_FORMAT_2
#undef TT_CONFIG_CMAP_FORMAT_14
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK

/* The TrueType bytecode interpreter never runs: FreeType's tt_glyph_load
 * honours FT_LOAD_NO_HINTING for every font, tricky ones included, whenever
 * FT_LOAD_NO_AUTOHINT is also set, and src/font.c always sets both. Unhinted
 * outlines and the unhinted size metrics src/font.c reads are computed the
 * same way without it; the face simply stops reading `cvt', `fpgm' and
 * `prep', which only the interpreter consumes. */
#undef TT_CONFIG_OPTION_BYTECODE_INTERPRETER
#undef TT_CONFIG_OPTION_SUBPIXEL_HINTING
/* ftoption.h derives these from the two options above before this file can
 * undefine them, so they have to be withdrawn too. */
#undef TT_USE_BYTECODE_INTERPRETER
#undef TT_SUPPORT_SUBPIXEL_HINTING_MINIMAL

/* EOF */
