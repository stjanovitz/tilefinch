/* Minimal FreeType module registration for Tilefinch page fonts.
 *
 * Overrides FreeType's default include/freetype/config/ftmodule.h through the
 * FT_CONFIG_MODULES_H compile definition set in cmake/freetype_vendored.cmake.
 * The same header is used for the host and the PSP cross-build, so
 * FT_Add_Default_Modules() registers an identical driver/renderer set on every
 * target (the M3 determinism discipline: host and device glyph rasters must be
 * bit-comparable).
 *
 * Kept:
 *   - sfnt    : SFNT table access and the Unicode cmap.
 *   - truetype: the `glyf' outline loader.
 *   - smooth  : the anti-aliased rasterizer.
 *
 * src/font.c loads glyphs with FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT and
 * renders FT_RENDER_MODE_NORMAL, so the auto-hinter (autofit), the PostScript
 * hinter/names, and the CFF/Type1/Type42/CID/PFR/BDF/PCF/winfonts drivers plus
 * the raster1/sdf/svg renderers are all unnecessary and are omitted here.  With
 * them unreferenced, the linker drops their objects from every final binary.
 *
 * WOFF1 uses the browser's zlib through FT_CONFIG_OPTION_USE_ZLIB. WOFF2 uses
 * the pinned, decode-only Brotli target and capped FreeType allocator. These
 * container decoders are independent of the module registration below.
 */

FT_USE_MODULE( FT_Module_Class,    sfnt_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Renderer_Class,  ft_smooth_renderer_class )

/* EOF */
