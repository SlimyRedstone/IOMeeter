#include "osd.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- shared --- */

static char s_error[160] = "not started";
static bool s_available;

/* Pushed down by osd_configure(), never read back out of here. */
static bool           s_enabled = true;
static osd_position_t s_where = OSD_TOP_RIGHT;
static int            s_ms = OSD_MS_DEFAULT;

/*
 * What the panel is showing, and until when.
 *
 * One panel at a time: a second press replaces the first rather than queueing
 * behind it, which is what a glance at the screen wants. The deadline is
 * milliseconds from the platform's own clock, so this file does not have to
 * know raylib exists.
 */
typedef struct {
    char  title[OSD_TEXT_MAX];
    char  detail[OSD_TEXT_MAX];   /*!< the reading, only set with has_bar */
    bool  has_bar;
    float level;                /*!< 0..1, only looked at with has_bar */
} osd_content_t;

static osd_content_t s_content;
static bool          s_showing;
static unsigned long long s_until;      /* milliseconds, 0 when down */

/*
 * The panel is two different things, so it is two different sizes, and the
 * backend works its own out rather than being handed one.
 *
 * A key press is a single line of text and the box is cut to fit it: a name is
 * anything from "Mute" to a sentence, and one fixed width either wastes most
 * of the box or clips the name.
 *
 * A fader is the interface's own widget at half the height -- an upright track
 * with the knob on it and the name set lengthwise inside the bottom, which is
 * the shape the eye already knows from the window. The numbers are fader.c's
 * own, copied rather than included: fader.h pulls in Clay and raylib, and this
 * file is deliberately free of both.
 */
#define OSD_PAD         16      /* from the edge of the panel */
#define OSD_MARGIN      24      /* from the edge of the screen */
#define OSD_RADIUS      14

/* One line of text, as wide as the line measures at the moment it goes up. */
#define OSD_TITLE_H     28
#define OSD_H_TEXT      (OSD_PAD + OSD_TITLE_H + OSD_PAD)
#define OSD_W_TEXT_MIN  140
#define OSD_W_TEXT_MAX  520

/* fader.c's TRACK_WIDTH, KNOB_WIDTH, KNOB_HEIGHT, KNOB_BORDER and
   LABEL_INSET, with its FADER_TRACK_HEIGHT of 325 halved. */
#define OSD_TRACK_W     22
#define OSD_KNOB_W      42
#define OSD_KNOB_H      21
#define OSD_KNOB_EDGE    3
#define OSD_TRACK_H    162
#define OSD_LABEL_INSET 16

/* The reading above the track, which the window's fader does without: there it
   is one of four and the eye compares the knobs, here it is on its own. */
#define OSD_READ_H      22
#define OSD_READ_GAP     6

#define OSD_W_BAR       (OSD_KNOB_W + 2 * OSD_PAD)
#define OSD_H_BAR       (OSD_PAD + OSD_READ_H + OSD_READ_GAP + \
                         OSD_TRACK_H + OSD_PAD)

/* Top of the track within the panel. */
#define OSD_TRACK_Y     (OSD_PAD + OSD_READ_H + OSD_READ_GAP)

/* ------------------------------------------------------------ naming ---- */

static const struct {
    const char *label;
    const char *id;
} s_positions[OSD_POSITION_COUNT] = {
    { "Top left",      "top_left"      },
    { "Top center",    "top_center"    },
    { "Top right",     "top_right"     },
    { "Bottom left",   "bottom_left"   },
    { "Bottom center", "bottom_center" },
    { "Bottom right",  "bottom_right"  },
};

static bool position_valid(osd_position_t where)
{
    return (int)where >= 0 && (int)where < OSD_POSITION_COUNT;
}

const char *osd_position_label(osd_position_t where)
{
    return s_positions[position_valid(where) ? where : OSD_TOP_RIGHT].label;
}

const char *osd_position_id(osd_position_t where)
{
    return s_positions[position_valid(where) ? where : OSD_TOP_RIGHT].id;
}

osd_position_t osd_position_parse(const char *name)
{
    if (name != NULL) {
        for (int i = 0; i < OSD_POSITION_COUNT; i++) {
            if (strcmp(name, s_positions[i].id) == 0) {
                return (osd_position_t)i;
            }
        }
    }
    return OSD_TOP_RIGHT;
}

int osd_clamp_ms(int ms)
{
    if (ms < OSD_MS_MIN) {
        return OSD_MS_MIN;
    }
    if (ms > OSD_MS_MAX) {
        return OSD_MS_MAX;
    }
    return ms;
}

bool osd_available(void)
{
    return s_available;
}

const char *osd_last_error(void)
{
    return s_error;
}

void osd_configure(bool enabled, osd_position_t where, int ms)
{
    s_enabled = enabled;
    s_where = position_valid(where) ? where : OSD_TOP_RIGHT;
    s_ms = osd_clamp_ms(ms);
}

/*
 * The panel a measured line of text needs.
 *
 * Shared because the measuring differs between the backends but what is done
 * with the answer does not. The minimum stops a two-letter name becoming a box
 * too small to read as a box; the maximum stops a pasted sentence spanning the
 * screen, and the drawing ends in an ellipsis so what is over is cut rather
 * than spilled.
 */
static int osd_text_panel_w(int text_w)
{
    int w = text_w + 2 * OSD_PAD;

    if (w < OSD_W_TEXT_MIN) {
        w = OSD_W_TEXT_MIN;
    }
    if (w > OSD_W_TEXT_MAX) {
        w = OSD_W_TEXT_MAX;
    }
    return w;
}

/*
 * Where the panel sits, in screen coordinates.
 *
 * The one piece of this that is a matter of taste rather than of what the
 * platform allows, and the same answer serves both backends.
 *
 * @param screen_w Width of the screen the panel goes on.
 * @param screen_h Its height.
 * @param panel_w  Width of the panel, which the text sets and a fader fixes.
 * @param panel_h  Its height, which depends on which of the two it is.
 * @param out_x    Receives the left edge.
 * @param out_y    Receives the top edge.
 */
static void osd_place(osd_position_t where, int screen_w, int screen_h,
                      int panel_w, int panel_h, int *out_x, int *out_y)
{
    switch (where) {
    case OSD_TOP_CENTER:
    case OSD_BOTTOM_CENTER:
        /* No margin on this axis: centred is centred, and the panel is far
           narrower than any screen it will be put on. */
        *out_x = (screen_w - panel_w) / 2;
        break;

    case OSD_TOP_RIGHT:
    case OSD_BOTTOM_RIGHT:
        *out_x = screen_w - panel_w - OSD_MARGIN;
        break;

    case OSD_TOP_LEFT:
    case OSD_BOTTOM_LEFT:
    default:
        *out_x = OSD_MARGIN;
        break;
    }

    switch (where) {
    case OSD_BOTTOM_LEFT:
    case OSD_BOTTOM_CENTER:
    case OSD_BOTTOM_RIGHT:
        /*
         * Anchored on its bottom edge rather than its top. The panel is two
         * shapes -- a fader tall and narrow, a key press short and wide -- so
         * a top-anchored panel at the bottom of the screen would jump up and
         * down as one kind of event followed the other.
         */
        *out_y = screen_h - panel_h - OSD_MARGIN;
        break;

    default:
        *out_y = OSD_MARGIN;
        break;
    }

    /* A screen smaller than the panel plus its margins is not worth a special
       case, but it must not put the panel off the top or left where there is
       no getting it back. */
    if (*out_x < 0) {
        *out_x = 0;
    }
    if (*out_y < 0) {
        *out_y = 0;
    }
}

/* Filled in by whichever backend is compiled, below. */
static unsigned long long osd_now_ms(void);
static void osd_backend_show(void);
static void osd_backend_hide(void);

/* ------------------------------------------------------------ showing --- */

/* The one way the panel goes up, whatever text is in it. */
static void osd_present(void)
{
    if (!s_available || !s_enabled) {
        return;
    }

    s_until = osd_now_ms() + (unsigned long long)s_ms;
    s_showing = true;

    osd_backend_show();
}

void osd_show_text(const char *title)
{
    if (!s_available || !s_enabled) {
        return;
    }

    snprintf(s_content.title, OSD_TEXT_MAX, "%s", title ? title : "");
    s_content.detail[0] = 0;        /* only a fader has a second thing to say */
    s_content.has_bar = false;
    s_content.level = 0.0f;

    osd_present();
}

void osd_show_level(const char *name, int value, int max)
{
    if (!s_available || !s_enabled) {
        return;
    }

    if (max <= 0) {
        max = 1;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > max) {
        value = max;
    }

    snprintf(s_content.title, OSD_TEXT_MAX, "%s", name ? name : "");
    snprintf(s_content.detail, OSD_TEXT_MAX, "%d%%",
             (value * 100 + max / 2) / max);

    s_content.has_bar = true;
    s_content.level = (float)value / (float)max;

    osd_present();
}

bool osd_visible(void)
{
    return s_showing;
}

void osd_tick(void)
{
    if (!s_showing) {
        return;
    }

    /* Also the way a disabled overlay puts away whatever was already up. */
    if (!s_enabled || osd_now_ms() >= s_until) {
        s_showing = false;
        s_until = 0;
        osd_backend_hide();
    }
}

/* ========================================================================== */
#ifdef _WIN32
/* ========================================================================== */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define OSD_CLASS "IOMeeterOverlay"

/* Opaque enough to read over a bright game, sheer enough not to hide it. */
#define OSD_ALPHA 225

static HWND  s_window;
static HFONT s_font_title;
static HFONT s_font_detail;
static HFONT s_font_label;

/* The size the rounded region was last cut for, so it is not recut on every
   frame of a fader moving. 0 for "none yet". */
static int   s_region_w;
static int   s_region_h;

static unsigned long long osd_now_ms(void)
{
    return (unsigned long long)GetTickCount64();
}

/* One flat rectangle. FillRect wants a brush, and the brush costs less than
   the bookkeeping to keep one of every colour around. */
static void osd_fill(HDC dc, int left, int top, int right, int bottom,
                     COLORREF colour)
{
    RECT r = { left, top, right, bottom };
    HBRUSH brush = CreateSolidBrush(colour);

    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

/* How wide the title is in the font it will be drawn in, for the box to be cut
   to. Measured against the window's own DC so the mapping matches the one the
   painting will use. */
static int osd_title_width(void)
{
    SIZE extent = { 0, 0 };
    HDC dc = GetDC(s_window);

    if (dc == NULL) {
        return OSD_W_TEXT_MIN;
    }

    HGDIOBJ old = SelectObject(dc, s_font_title);

    GetTextExtentPoint32A(dc, s_content.title, (int)strlen(s_content.title),
                          &extent);
    SelectObject(dc, old);
    ReleaseDC(s_window, dc);

    return (int)extent.cx;
}

/* A key press: the name, on one line, in a box cut to it. */
static void osd_paint_text(HDC dc, int w)
{
    RECT line = { OSD_PAD, OSD_PAD, w - OSD_PAD, OSD_PAD + OSD_TITLE_H };
    HGDIOBJ old = SelectObject(dc, s_font_title);

    SetTextColor(dc, RGB(0xe7, 0xe9, 0xee));
    DrawTextA(dc, s_content.title, -1, &line,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    SelectObject(dc, old);
}

/*
 * The name up the inside of the track, the way the window draws it.
 *
 * s_font_label carries a quarter-turn escapement, so the baseline runs up the
 * panel and the glyphs stand to the left of it: the band the text covers is
 * the ascent to the left of the baseline and the descent to the right, and
 * offsetting by half the difference between them centres that band on the
 * track rather than centring the baseline.
 */
static void osd_paint_label(HDC dc, int centre_x, int baseline_y)
{
    TEXTMETRICA tm;
    HGDIOBJ old = SelectObject(dc, s_font_label);

    GetTextMetricsA(dc, &tm);

    SetTextAlign(dc, TA_LEFT | TA_BASELINE);
    SetTextColor(dc, RGB(0x20, 0x3e, 0x2e));
    TextOutA(dc, centre_x + (tm.tmAscent - tm.tmDescent) / 2, baseline_y,
             s_content.title, (int)strlen(s_content.title));

    /* DrawTextA is only correct under TA_LEFT | TA_TOP, and it runs next. */
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    SelectObject(dc, old);
}

/*
 * A fader: the reading, the track, the name inside it and the knob.
 *
 * The colours are fader.c's, each one the midpoint of the pair it blends
 * between there. GDI has no gradient fill without linking msimg32, and across
 * a track 22 pixels wide the blend was never the point.
 */
static void osd_paint_fader(HDC dc, int w)
{
    int centre = w / 2;
    int left = centre - OSD_TRACK_W / 2;
    int right = left + OSD_TRACK_W;
    int top = OSD_TRACK_Y;
    int bottom = top + OSD_TRACK_H;

    /* The reading, centred above the track. */
    RECT read = { 0, OSD_PAD, w, OSD_PAD + OSD_READ_H };
    HGDIOBJ old = SelectObject(dc, s_font_detail);

    SetTextColor(dc, RGB(0xe7, 0xe9, 0xee));
    DrawTextA(dc, s_content.detail, -1, &read,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old);

    /* Travel inset by half the knob at each end, as fader.c's metrics are, so
       the knob is flush with the end of the track at either extreme rather
       than hanging off it. */
    int travel_top = top + OSD_KNOB_H / 2;
    int travel_bottom = bottom - OSD_KNOB_H / 2;
    int knob_y = travel_bottom -
                 (int)(s_content.level * (float)(travel_bottom - travel_top));
    int split = knob_y;

    if (split < top) {
        split = top;
    }
    if (split > bottom) {
        split = bottom;
    }

    /* Empty above the knob, filled below it. An empty rectangle draws nothing,
       so neither end of the travel needs a special case. */
    osd_fill(dc, left, top, right, split, RGB(0x4c, 0x54, 0x60));
    osd_fill(dc, left, top, left + 1, split, RGB(0x36, 0x3c, 0x46));
    osd_fill(dc, right - 1, top, right, split, RGB(0x36, 0x3c, 0x46));

    osd_fill(dc, left, split, right, bottom, RGB(0x8f, 0xd6, 0xab));
    osd_fill(dc, left, split, left + 1, bottom, RGB(0x60, 0xaa, 0x80));
    osd_fill(dc, right - 1, split, right, bottom, RGB(0x60, 0xaa, 0x80));

    /* Before the knob, so a fader near zero has the knob cover the name rather
       than collide with it. */
    osd_paint_label(dc, centre, bottom - OSD_LABEL_INSET);

    int knob_top = knob_y - OSD_KNOB_H / 2;

    osd_fill(dc, centre - OSD_KNOB_W / 2, knob_top,
             centre + OSD_KNOB_W / 2, knob_top + OSD_KNOB_H,
             RGB(0xb0, 0xec, 0xc8));
    osd_fill(dc, centre - OSD_KNOB_W / 2 + OSD_KNOB_EDGE,
             knob_top + OSD_KNOB_EDGE,
             centre + OSD_KNOB_W / 2 - OSD_KNOB_EDGE,
             knob_top + OSD_KNOB_H - OSD_KNOB_EDGE,
             RGB(0x6c, 0xbe, 0x91));
}

/*
 * Painted whole on every WM_PAINT: the panel is a few hundred pixels and the
 * content changes every time it is shown, so there is nothing worth keeping
 * between one and the next.
 */
static void osd_paint(HDC dc, const RECT *area)
{
    int w = area->right - area->left;

    HBRUSH back = CreateSolidBrush(RGB(0x16, 0x16, 0x18));
    FillRect(dc, area, back);
    DeleteObject(back);

    SetBkMode(dc, TRANSPARENT);

    if (s_content.has_bar) {
        osd_paint_fader(dc, w);
    } else {
        osd_paint_text(dc, w);
    }
}

static LRESULT CALLBACK osd_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT area;

        GetClientRect(hwnd, &area);
        osd_paint(dc, &area);
        EndPaint(hwnd, &ps);
        return 0;
    }

    /* Never takes the focus, however it is asked. A game that loses the focus
       to a notification is a game that minimises. */
    if (msg == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

bool osd_start(void)
{
    WNDCLASSEXA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = osd_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = OSD_CLASS;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);

    if (RegisterClassExA(&wc) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        snprintf(s_error, sizeof(s_error),
                 "overlay class could not be registered (%lu)",
                 (unsigned long)GetLastError());
        return false;
    }

    /*
     * LAYERED for the transparency, TRANSPARENT so the pointer goes straight
     * through to whatever is underneath, NOACTIVATE so clicking near it does
     * not pull the focus off a game, TOOLWINDOW to keep it out of alt-tab and
     * off the taskbar, and TOPMOST to put it over everything that is not
     * exclusively fullscreen.
     *
     * The size here is only what it is created at, hidden. Every show sets it
     * from what is being shown.
     */
    s_window = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        OSD_CLASS, "", WS_POPUP,
        0, 0, OSD_W_BAR, OSD_H_BAR,
        NULL, NULL, wc.hInstance, NULL);

    if (s_window == NULL) {
        snprintf(s_error, sizeof(s_error),
                 "overlay window could not be created (%lu)",
                 (unsigned long)GetLastError());
        return false;
    }

    /* One alpha for the whole window. Per-pixel alpha would mean painting into
       a DIB and pushing it with UpdateLayeredWindow, and GDI's text drawing
       leaves the alpha channel at zero, so every glyph would have to be fixed
       up afterwards. A rounded region gives the shape for far less. */
    SetLayeredWindowAttributes(s_window, 0, OSD_ALPHA, LWA_ALPHA);

    s_font_title = CreateFontA(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    s_font_detail = CreateFontA(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    /*
     * The fader's name, turned a quarter turn. Arguments three and four are
     * the escapement and the orientation, in tenths of a degree, and 900 in
     * both is what makes this work in either graphics mode: under
     * GM_COMPATIBLE the escapement sets both anyway.
     *
     * A real bold face, rather than the four offset passes fader.c resorts to
     * for want of a bold among the fonts raylib has loaded.
     */
    s_font_label = CreateFontA(-22, 0, 900, 900, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    s_available = true;
    s_error[0] = 0;
    return true;
}

void osd_stop(void)
{
    if (s_window != NULL) {
        DestroyWindow(s_window);
        s_window = NULL;
    }
    if (s_font_title != NULL) {
        DeleteObject(s_font_title);
        s_font_title = NULL;
    }
    if (s_font_detail != NULL) {
        DeleteObject(s_font_detail);
        s_font_detail = NULL;
    }
    if (s_font_label != NULL) {
        DeleteObject(s_font_label);
        s_font_label = NULL;
    }
    s_available = false;
    s_showing = false;
    s_region_w = 0;
    s_region_h = 0;
}

static void osd_backend_show(void)
{
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x = 0;
    int y = 0;

    /* A fader is one shape whatever it reads; a key press is as wide as its
       name, which has to be measured before there is a box to put it in. */
    int panel_w = s_content.has_bar ? OSD_W_BAR
                                    : osd_text_panel_w(osd_title_width());
    int panel_h = s_content.has_bar ? OSD_H_BAR : OSD_H_TEXT;

    osd_place(s_where, screen_w, screen_h, panel_w, panel_h, &x, &y);

    /*
     * The region is remade with the size, because a region cut for one of the
     * panel's two shapes clips the other -- but only when the size has
     * actually changed. This runs on every frame a fader is moving, and
     * SetWindowRgn redraws the whole window whether or not the shape it is
     * given differs from the one already set.
     */
    if (panel_w != s_region_w || panel_h != s_region_h) {
        HRGN round = CreateRoundRectRgn(0, 0, panel_w + 1, panel_h + 1,
                                        OSD_RADIUS, OSD_RADIUS);

        SetWindowRgn(s_window, round, FALSE);   /* the window owns it now */
        s_region_w = panel_w;
        s_region_h = panel_h;
    }

    /*
     * Topmost is re-asserted on every show rather than trusted from creation:
     * a game going fullscreen puts itself at the top of the z-order, and a
     * window that was topmost before it started is not above it afterwards.
     * SWP_NOACTIVATE for the same reason WS_EX_NOACTIVATE is set.
     */
    SetWindowPos(s_window, HWND_TOPMOST, x, y, panel_w, panel_h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    InvalidateRect(s_window, NULL, TRUE);
    UpdateWindow(s_window);
}

static void osd_backend_hide(void)
{
    ShowWindow(s_window, SW_HIDE);
}

/* ========================================================================== */
#elif defined(OSD_HAVE_X11)
/* ========================================================================== */

#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>

/*
 * The name goes under the track here rather than up the inside of it: a core X
 * font cannot be turned on its side, and turning one would mean Xft and
 * fontconfig linked for the sake of a single word. The panel is a row taller
 * than the Windows one to hold it.
 */
#define OSD_LABEL_H 22

static Display *s_display;
static Window   s_window;
static GC       s_gc;
static XFontStruct *s_font;
static int      s_screen;
static bool     s_mapped;

static unsigned long long osd_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000L);
}

/* A colour by value rather than by name: the X colour database is not
   something to depend on for a handful of fixed shades. */
static unsigned long osd_pixel(int r, int g, int b)
{
    XColor c;

    c.red   = (unsigned short)(r * 257);
    c.green = (unsigned short)(g * 257);
    c.blue  = (unsigned short)(b * 257);
    c.flags = DoRed | DoGreen | DoBlue;

    if (!XAllocColor(s_display, DefaultColormap(s_display, s_screen), &c)) {
        return BlackPixel(s_display, s_screen);
    }
    return c.pixel;
}

static int osd_font_ascent(void)
{
    return s_font ? s_font->ascent : 12;
}

static int osd_text_width(const char *text)
{
    int len = (int)strlen(text);

    return s_font ? XTextWidth(s_font, text, len) : len * 6;
}

static void osd_fill(int x, int y, int w, int h, int r, int g, int b)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    XSetForeground(s_display, s_gc, osd_pixel(r, g, b));
    XFillRectangle(s_display, s_window, s_gc, x, y, (unsigned)w, (unsigned)h);
}

/* The same fader as the other backend, down to fader.c's colours, with the
   name below the track instead of inside it. */
static void osd_paint_fader(int panel_w)
{
    int centre = panel_w / 2;
    int left = centre - OSD_TRACK_W / 2;
    int top = OSD_TRACK_Y;
    int bottom = top + OSD_TRACK_H;

    /* The reading, centred above the track. */
    int read_w = osd_text_width(s_content.detail);

    XSetForeground(s_display, s_gc, osd_pixel(0xe7, 0xe9, 0xee));
    XDrawString(s_display, s_window, s_gc, centre - read_w / 2,
                OSD_PAD + osd_font_ascent(),
                s_content.detail, (int)strlen(s_content.detail));

    /* Travel inset by half the knob at each end, as fader.c's metrics are. */
    int travel_top = top + OSD_KNOB_H / 2;
    int travel_bottom = bottom - OSD_KNOB_H / 2;
    int knob_y = travel_bottom -
                 (int)(s_content.level * (float)(travel_bottom - travel_top));
    int split = knob_y;

    if (split < top) {
        split = top;
    }
    if (split > bottom) {
        split = bottom;
    }

    osd_fill(left, top, OSD_TRACK_W, split - top, 0x4c, 0x54, 0x60);
    osd_fill(left, top, 1, split - top, 0x36, 0x3c, 0x46);
    osd_fill(left + OSD_TRACK_W - 1, top, 1, split - top, 0x36, 0x3c, 0x46);

    osd_fill(left, split, OSD_TRACK_W, bottom - split, 0x8f, 0xd6, 0xab);
    osd_fill(left, split, 1, bottom - split, 0x60, 0xaa, 0x80);
    osd_fill(left + OSD_TRACK_W - 1, split, 1, bottom - split,
             0x60, 0xaa, 0x80);

    int knob_top = knob_y - OSD_KNOB_H / 2;

    osd_fill(centre - OSD_KNOB_W / 2, knob_top, OSD_KNOB_W, OSD_KNOB_H,
             0xb0, 0xec, 0xc8);
    osd_fill(centre - OSD_KNOB_W / 2 + OSD_KNOB_EDGE,
             knob_top + OSD_KNOB_EDGE,
             OSD_KNOB_W - 2 * OSD_KNOB_EDGE,
             OSD_KNOB_H - 2 * OSD_KNOB_EDGE,
             0x6c, 0xbe, 0x91);

    /* The name, centred under the track. */
    int name_w = osd_text_width(s_content.title);

    XSetForeground(s_display, s_gc, osd_pixel(0xe7, 0xe9, 0xee));
    XDrawString(s_display, s_window, s_gc, centre - name_w / 2,
                bottom + 2 + osd_font_ascent(),
                s_content.title, (int)strlen(s_content.title));
}

static void osd_paint(int panel_w, int panel_h)
{
    osd_fill(0, 0, panel_w, panel_h, 0x16, 0x16, 0x18);

    if (s_content.has_bar) {
        osd_paint_fader(panel_w);
    } else {
        XSetForeground(s_display, s_gc, osd_pixel(0xe7, 0xe9, 0xee));
        XDrawString(s_display, s_window, s_gc, OSD_PAD,
                    OSD_PAD + osd_font_ascent(),
                    s_content.title, (int)strlen(s_content.title));
    }

    XFlush(s_display);
}

bool osd_start(void)
{
    s_display = XOpenDisplay(NULL);
    if (s_display == NULL) {
        snprintf(s_error, sizeof(s_error),
                 "no X display for the overlay; a native Wayland session has "
                 "no way for an unprivileged program to draw over others");
        return false;
    }

    s_screen = DefaultScreen(s_display);

    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));

    /* override_redirect is what keeps the window manager's hands off it: no
       frame, no placement of its own, no entry in the task list. */
    attr.override_redirect = True;
    attr.background_pixel = BlackPixel(s_display, s_screen);
    attr.event_mask = ExposureMask;

    s_window = XCreateWindow(s_display, RootWindow(s_display, s_screen),
                             0, 0, OSD_W_BAR, OSD_H_BAR + OSD_LABEL_H, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask,
                             &attr);

    if (s_window == 0) {
        snprintf(s_error, sizeof(s_error), "overlay window could not be created");
        XCloseDisplay(s_display);
        s_display = NULL;
        return false;
    }

    /* Typed and stacked for the compositors that read these even on an
       override-redirect window, which costs nothing where they do not. */
    Atom type = XInternAtom(s_display, "_NET_WM_WINDOW_TYPE", False);
    Atom notification = XInternAtom(s_display,
                                    "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    XChangeProperty(s_display, s_window, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&notification, 1);

    Atom state = XInternAtom(s_display, "_NET_WM_STATE", False);
    Atom above = XInternAtom(s_display, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(s_display, s_window, state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&above, 1);

    /* An empty input region: the pointer finds whatever is underneath, so a
       panel over a game's crosshair costs nothing. */
    XShapeCombineRectangles(s_display, s_window, ShapeInput, 0, 0, NULL, 0,
                            ShapeSet, Unsorted);

    s_gc = XCreateGC(s_display, s_window, 0, NULL);

    /* A core font, so there is no Xft or fontconfig to link for two lines. */
    s_font = XLoadQueryFont(s_display, "-*-helvetica-bold-r-*-*-17-*-*-*-*-*-*-*");
    if (s_font == NULL) {
        s_font = XLoadQueryFont(s_display, "fixed");
    }
    if (s_font != NULL) {
        XSetFont(s_display, s_gc, s_font->fid);
    }

    s_available = true;
    s_error[0] = 0;
    return true;
}

void osd_stop(void)
{
    if (s_display == NULL) {
        return;
    }
    if (s_font != NULL) {
        XFreeFont(s_display, s_font);
        s_font = NULL;
    }
    if (s_gc != NULL) {
        XFreeGC(s_display, s_gc);
        s_gc = NULL;
    }
    if (s_window != 0) {
        XDestroyWindow(s_display, s_window);
        s_window = 0;
    }

    XCloseDisplay(s_display);
    s_display = NULL;
    s_available = false;
    s_showing = false;
    s_mapped = false;
}

static void osd_backend_show(void)
{
    int x = 0;
    int y = 0;
    int panel_w;
    int panel_h;

    if (s_content.has_bar) {
        /* Wide enough for the knob, or for the name under it, whichever asks
           for more: the name is horizontal here, and a long one would
           otherwise run off both sides of the panel. The knob is the floor
           rather than the text minimum, a fader being narrow by design.  */
        panel_w = osd_text_width(s_content.title) + 2 * OSD_PAD;

        if (panel_w < OSD_W_BAR) {
            panel_w = OSD_W_BAR;
        }
        if (panel_w > OSD_W_TEXT_MAX) {
            panel_w = OSD_W_TEXT_MAX;
        }
        panel_h = OSD_H_BAR + OSD_LABEL_H;
    } else {
        panel_w = osd_text_panel_w(osd_text_width(s_content.title));
        panel_h = OSD_H_TEXT;
    }

    osd_place(s_where, DisplayWidth(s_display, s_screen),
              DisplayHeight(s_display, s_screen), panel_w, panel_h, &x, &y);

    XMoveResizeWindow(s_display, s_window, x, y, (unsigned)panel_w,
                      (unsigned)panel_h);

    if (!s_mapped) {
        XMapWindow(s_display, s_window);
        s_mapped = true;
    }

    /* Raised on every show for the same reason Windows re-asserts topmost:
       whatever went fullscreen since the last one is above this now. */
    XRaiseWindow(s_display, s_window);
    osd_paint(panel_w, panel_h);
}

static void osd_backend_hide(void)
{
    if (s_mapped) {
        XUnmapWindow(s_display, s_window);
        s_mapped = false;
        XFlush(s_display);
    }
}

/* ========================================================================== */
#else
/* ========================================================================== */

static unsigned long long osd_now_ms(void) { return 0; }
static void osd_backend_hide(void) {}

/* Calls the two pieces of this file that are neither platform's, so that they
   keep being compiled where neither is built. */
static void osd_backend_show(void)
{
    int x = 0;
    int y = 0;

    osd_place(s_where, 0, 0, osd_text_panel_w(0),
              s_content.has_bar ? OSD_H_BAR : OSD_H_TEXT, &x, &y);
}

bool osd_start(void)
{
    snprintf(s_error, sizeof(s_error),
             "built without X11, so there is no overlay");
    return false;
}

void osd_stop(void) {}

#endif /* _WIN32 */
