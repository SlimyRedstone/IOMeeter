/*
 * Type sizes for the whole interface, in pixels.
 *
 * Definitions only: nothing here allocates, loads or measures anything. The
 * fonts themselves are loaded in ui.c, which is also where the two face ids
 * live; this is only the scale they are drawn at, gathered so a change is one
 * edit rather than forty.
 *
 * The scale starts at FONT_SIZE_SMALL and climbs in steps from there. Sizes
 * are handed to raylib as they stand, so a value above FONT_SIZE_ATLAS is
 * drawn from a glyph atlas smaller than itself and will look soft: raise the
 * atlas alongside anything that outgrows it.
 */

#ifndef FONTS_H
#define FONTS_H

/* Lists, captions, key labels, the traffic console: everything dense. */
#define FONT_SIZE_SMALL     18

#define FONT_SIZE_APP_LIST  14

/* Card headings and panel titles. */
#define FONT_SIZE_CAPTION   16

/* Running text, field contents, profile names. */
#define FONT_SIZE_BODY      20

/* Buttons and pickable rows, a step up so they read as targets. */
#define FONT_SIZE_ITEM      20

/* The interrupt toast, which has to carry across the window. */
#define FONT_SIZE_NOTICE    23

/* The application name in the header. */
#define FONT_SIZE_TITLE     26

/*
 * Size the glyph atlases are rasterised at. Comfortably above the largest
 * size above, since scaling a glyph down stays sharp and scaling it up does
 * not.
 */
#define FONT_SIZE_ATLAS     40

#endif /* FONTS_H */
