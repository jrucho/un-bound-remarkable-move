// chromium_viewer: show a rendered page frame through quill's verified color
// path with no browser chrome or visible corner affordances.
//
// Interactive mode (4th arg = FIFO path): the viewer becomes the input half
// of a browser. It watches the image file and repaints when the CDP driver
// atomically renames a new frame over it, and it forwards gestures as text
// lines on the FIFO: "TAP x y" and "SWIPE x1 y1 x2 y2" in screen pixels.
//
// On-screen keyboard (ported from paperterm's osk.rs): swipe two fingers up
// to raise it, then use its "hide" key to drop it. Keys go to the driver as
// "KEY <codepoint> <mods>" / "KEYN <name> <mods>" lines and the page viewport
// is shrunk via "VP <w> <h>" so the composer stays visible above the band.
//
// Two fingers down requests a cleaning full refresh. Exits on: power button,
// 5-finger tap, SIGTERM.
// Usage: chromium_viewer /path/to/page.png [fit|fill|stretch] [full] [fifo]

#include <QImage>
#include <QtGlobal>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/inotify.h>
#include <libgen.h>
#include <limits.h>

extern "C" {
#include "quill.h"
}

#include "osk_font.h"   // spleen-16x32.psfu (paperterm), xxd -i

#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define ABS_X 0
#define ABS_Y 1
#define BTN_TOOL_PEN 320
#define BTN_TOOL_RUBBER 321
#define BTN_TOUCH 330
#define ABS_MT_SLOT 47
#define ABS_MT_POSITION_X 53
#define ABS_MT_POSITION_Y 54
#define ABS_MT_TRACKING_ID 57
#define KEY_POWER 116
#define EVIOCGRAB 0x40044590
#define MAX_SLOTS 16

struct input_event { struct timeval time; uint16_t type; uint16_t code; int32_t value; };
struct input_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
#define EVIOCGABS(abs) (0x80000000 | (sizeof(struct input_absinfo) << 16) | ('E' << 8) | (0x40 + (abs)))

static volatile sig_atomic_t g_quit = 0;
static void on_term(int sig) { (void)sig; g_quit = 1; }

static int W, H, STRIDE, BPP;
static unsigned char *FB;
static int TOUCH_MAX_X = 0, TOUCH_MAX_Y = 0;
#define DIGI_MAX_X 11180
#define DIGI_MAX_Y 15340
static int FLIP_X = 0, FLIP_Y = 0;   // CHROMIUM_TOUCH_FLIP: "x", "y", "xy"
static int FIFO_FD = -1;
#define CORNER_PX 220   // tap target, >= the spec's 200 px minimum
#define TAP_MAX_DIST 40 // finger travel below this = tap, above = swipe
#define TWO_FINGER_MIN_DIST 160

static void put_rgb(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = FB + (size_t)y * STRIDE + (size_t)x * BPP;
    if (BPP == 4) { p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF; }
    else if (BPP == 2) { uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); p[0] = v & 255; p[1] = v >> 8; }
    else memset(p, (30 * r + 59 * g + 11 * b) / 100, BPP);
}

#define CUR_R 12
static unsigned char cur_save[(2*CUR_R+2) * (2*CUR_R+2) * 4];
static int cur_x = -1, cur_y = -1;   // top-left of saved rect, -1 = nothing saved

static void rect_clamp(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > W) *w = W - *x;
    if (*y + *h > H) *h = H - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static void cursor_restore(void) {
    if (cur_x < 0) return;
    int x = cur_x, y = cur_y, w = 2*CUR_R+2, h = 2*CUR_R+2;
    rect_clamp(&x, &y, &w, &h);
    for (int r = 0; r < h; r++)
        memcpy(FB + (size_t)(y+r)*STRIDE + (size_t)x*BPP,
               cur_save + (size_t)((y+r)-cur_y)*(2*CUR_R+2)*BPP + (size_t)(x-cur_x)*BPP,
               (size_t)w*BPP);
    quill_swap_ex(x, y, w, h, QUILL_MODE_COLOR4, 0, QUILL_CONTENT_COLOR);
    cur_x = cur_y = -1;
}

// Draw a small ring (black with white inner edge) centered at (px,py); saves
// the pixels underneath so it can be erased without repainting the page.
static void cursor_draw(int px, int py) {
    int x = px - CUR_R - 1, y = py - CUR_R - 1, w = 2*CUR_R+2, h = 2*CUR_R+2;
    cur_x = x; cur_y = y;
    int cx = x, cy = y, cw = w, ch = h;
    rect_clamp(&cx, &cy, &cw, &ch);
    for (int r = 0; r < ch; r++)
        memcpy(cur_save + (size_t)((cy+r)-cur_y)*(2*CUR_R+2)*BPP + (size_t)(cx-cur_x)*BPP,
               FB + (size_t)(cy+r)*STRIDE + (size_t)cx*BPP,
               (size_t)cw*BPP);
    for (int dy = -CUR_R; dy <= CUR_R; dy++)
        for (int dx = -CUR_R; dx <= CUR_R; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 <= CUR_R*CUR_R && d2 >= (CUR_R-4)*(CUR_R-4)) {
                int inner = d2 <= (CUR_R-2)*(CUR_R-2);
                put_rgb(px+dx, py+dy, inner?255:0, inner?255:0, inner?255:0);
            }
        }
    quill_swap_ex(cx, cy, cw, ch, QUILL_MODE_COLOR4, 0, QUILL_CONTENT_COLOR);
}

static void send_line(const char *line) {
    if (FIFO_FD < 0) return;
    ssize_t n = write(FIFO_FD, line, strlen(line));
    if (n < 0) { close(FIFO_FD); FIFO_FD = -1; }   // reader died: reconnect later
}

// ---------- PSF2 font (embedded Spleen 16x32, parser ported from paperterm) ----------

static struct {
    int w, h, charsize, bpr;
    const unsigned char *glyphs;   // NULL = font failed to parse, OSK disabled
    int map[128];                  // ASCII -> glyph index
} FONT;

static int font_init(void) {
    const unsigned char *d = OSK_FONT;
    unsigned n = OSK_FONT_len;
    if (n < 32 || d[0] != 0x72 || d[1] != 0xB5 || d[2] != 0x4A || d[3] != 0x86) return -1;
    unsigned u32[8];
    for (int i = 0; i < 8; i++)
        u32[i] = d[i*4] | d[i*4+1] << 8 | d[i*4+2] << 16 | d[i*4+3] << 24;
    unsigned headersize = u32[2], flags = u32[3], length = u32[4];
    unsigned charsize = u32[5], height = u32[6], width = u32[7];
    unsigned bpr = (width + 7) / 8;
    unsigned bitmaps_end = headersize + length * charsize;
    if (charsize != bpr * height || n < bitmaps_end) return -1;
    FONT.w = width; FONT.h = height; FONT.charsize = charsize; FONT.bpr = bpr;
    memset(FONT.map, 0, sizeof FONT.map);
    if (flags & 1) {
        // Unicode table: UTF-8 codepoints per glyph, 0xFF terminates a glyph,
        // 0xFE starts combining sequences (skip to terminator).
        unsigned pos = bitmaps_end, glyph = 0;
        while (pos < n && glyph < length) {
            unsigned char b = d[pos];
            if (b == 0xFF) { glyph++; pos++; }
            else if (b == 0xFE) { while (pos < n && d[pos] != 0xFF) pos++; }
            else {
                int len = b < 0x80 ? 1 : (b >= 0xC0 && b < 0xE0) ? 2 : (b >= 0xE0 && b < 0xF0) ? 3 : 4;
                if (len == 1 && b >= 32 && b < 128 && FONT.map[b] == 0) FONT.map[b] = glyph;
                pos += len;
            }
        }
    } else {
        for (unsigned i = 32; i < length && i < 128; i++) FONT.map[i] = i;
    }
    FONT.glyphs = d + headersize;
    return 0;
}

static void draw_glyphs(int x, int y, const char *s, unsigned char lum) {
    for (; *s; s++, x += FONT.w) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        const unsigned char *g = FONT.glyphs + FONT.map[c] * FONT.charsize;
        for (int r = 0; r < FONT.h; r++)
            for (int cx = 0; cx < FONT.w; cx++)
                if (g[r * FONT.bpr + (cx >> 3)] & (0x80 >> (cx & 7)))
                    put_rgb(x + cx, y + r, lum, lum, lum);
    }
}

// ---------- on-screen keyboard (layout + behavior from paperterm's osk.rs) ----------

enum { KMOD_ALT = 1, KMOD_CTRL = 2, KMOD_SHIFT = 8 };        // CDP modifier bits
enum { MOD_OFF, MOD_ONESHOT, MOD_LOCKED };                   // tap: oneshot -> locked -> off
enum { KK_CHAR, KK_NAMED, KK_SHIFT, KK_CTRL, KK_ALT, KK_RFSH, KK_HIDE, KK_URL, KK_BACK };

struct OskDef { float u; int kind; char ch, shifted; const char *name; const char *label; };
struct OskKey { int kind; char ch, shifted; const char *name; const char *label; int x, y, w, h; };

#define OSK_ROWS 5
#define OSK_ROW_H 110
#define OSK_H (OSK_ROWS * OSK_ROW_H)
static OskKey osk_keys[80];
static int osk_nkeys = 0;
static int osk_y0 = 0;
static int osk_visible = 0;
static int osk_shift = MOD_OFF, osk_ctrl = MOD_OFF, osk_alt = MOD_OFF;
static int osk_touch_pressed = -1, osk_pen_pressed = -1;     // key idx per input source

static const char *g_img_path = NULL, *g_img_mode = "stretch";
static int display_file(const char *path, const char *mode, int full);
static void osk_uncursor(void);   // defined with the pen state below

static void osk_init(void) {
    static const OskDef r1[] = {
        {1.3f, KK_NAMED, 0, 0, "esc", "esc"}, {1, KK_CHAR, '`', '~', 0, 0},
        {1, KK_CHAR, '1', '!', 0, 0}, {1, KK_CHAR, '2', '@', 0, 0}, {1, KK_CHAR, '3', '#', 0, 0},
        {1, KK_CHAR, '4', '$', 0, 0}, {1, KK_CHAR, '5', '%', 0, 0}, {1, KK_CHAR, '6', '^', 0, 0},
        {1, KK_CHAR, '7', '&', 0, 0}, {1, KK_CHAR, '8', '*', 0, 0}, {1, KK_CHAR, '9', '(', 0, 0},
        {1, KK_CHAR, '0', ')', 0, 0}, {1, KK_CHAR, '-', '_', 0, 0}, {1, KK_CHAR, '=', '+', 0, 0},
        {1.7f, KK_NAMED, 0, 0, "backspace", "bksp"},
    };
    static const OskDef r2[] = {
        {1.5f, KK_NAMED, 0, 0, "tab", "tab"},
        {1, KK_CHAR, 'q', 'Q', 0, 0}, {1, KK_CHAR, 'w', 'W', 0, 0}, {1, KK_CHAR, 'e', 'E', 0, 0},
        {1, KK_CHAR, 'r', 'R', 0, 0}, {1, KK_CHAR, 't', 'T', 0, 0}, {1, KK_CHAR, 'y', 'Y', 0, 0},
        {1, KK_CHAR, 'u', 'U', 0, 0}, {1, KK_CHAR, 'i', 'I', 0, 0}, {1, KK_CHAR, 'o', 'O', 0, 0},
        {1, KK_CHAR, 'p', 'P', 0, 0}, {1, KK_CHAR, '[', '{', 0, 0}, {1, KK_CHAR, ']', '}', 0, 0},
        {1.5f, KK_CHAR, '\\', '|', 0, 0},
    };
    static const OskDef r3[] = {
        {1.8f, KK_CTRL, 0, 0, 0, "ctrl"},
        {1, KK_CHAR, 'a', 'A', 0, 0}, {1, KK_CHAR, 's', 'S', 0, 0}, {1, KK_CHAR, 'd', 'D', 0, 0},
        {1, KK_CHAR, 'f', 'F', 0, 0}, {1, KK_CHAR, 'g', 'G', 0, 0}, {1, KK_CHAR, 'h', 'H', 0, 0},
        {1, KK_CHAR, 'j', 'J', 0, 0}, {1, KK_CHAR, 'k', 'K', 0, 0}, {1, KK_CHAR, 'l', 'L', 0, 0},
        {1, KK_CHAR, ';', ':', 0, 0}, {1, KK_CHAR, '\'', '"', 0, 0},
        {1.9f, KK_NAMED, 0, 0, "enter", "enter"},
    };
    static const OskDef r4[] = {
        {2.0f, KK_SHIFT, 0, 0, 0, "shift"},
        {1, KK_CHAR, 'z', 'Z', 0, 0}, {1, KK_CHAR, 'x', 'X', 0, 0}, {1, KK_CHAR, 'c', 'C', 0, 0},
        {1, KK_CHAR, 'v', 'V', 0, 0}, {1, KK_CHAR, 'b', 'B', 0, 0}, {1, KK_CHAR, 'n', 'N', 0, 0},
        {1, KK_CHAR, 'm', 'M', 0, 0}, {1, KK_CHAR, ',', '<', 0, 0}, {1, KK_CHAR, '.', '>', 0, 0},
        {1, KK_CHAR, '/', '?', 0, 0},
        {1.2f, KK_NAMED, 0, 0, "up", "^"}, {1.5f, KK_NAMED, 0, 0, "del", "del"},
    };
    static const OskDef r5[] = {
        {1.4f, KK_HIDE, 0, 0, 0, "hide"}, {1.3f, KK_BACK, 0, 0, 0, "back"},
        {1.2f, KK_URL, 0, 0, 0, "url"},
        {1.2f, KK_NAMED, 0, 0, "pgup", "pgup"}, {1.2f, KK_NAMED, 0, 0, "pgdn", "pgdn"},
        {3.8f, KK_CHAR, ' ', ' ', 0, 0},
        {1.1f, KK_NAMED, 0, 0, "left", "<"}, {1.1f, KK_NAMED, 0, 0, "down", "v"},
        {1.1f, KK_NAMED, 0, 0, "right", ">"}, {1.2f, KK_RFSH, 0, 0, 0, "rfsh"},
    };
    static const OskDef *rows[OSK_ROWS] = {r1, r2, r3, r4, r5};
    static const int counts[OSK_ROWS] = {15, 14, 13, 13, 10};
    osk_y0 = H - OSK_H;
    osk_nkeys = 0;
    for (int ri = 0; ri < OSK_ROWS; ri++) {
        float total = 0;
        for (int i = 0; i < counts[ri]; i++) total += rows[ri][i].u;
        float unit = (float)W / total, x = 0;
        for (int i = 0; i < counts[ri]; i++) {
            const OskDef *def = &rows[ri][i];
            OskKey *k = &osk_keys[osk_nkeys++];
            k->kind = def->kind; k->ch = def->ch; k->shifted = def->shifted;
            k->name = def->name; k->label = def->label;
            k->x = (int)(x + 0.5f);
            k->y = osk_y0 + ri * OSK_ROW_H;
            float w = def->u * unit;
            k->w = (int)(w + 0.5f);
            if (k->x + k->w > W) k->w = W - k->x;
            k->h = OSK_ROW_H;
            x += w;
        }
    }
}

static int osk_hit(int sx, int sy) {
    for (int i = 0; i < osk_nkeys; i++) {
        const OskKey *k = &osk_keys[i];
        if (sx >= k->x && sx < k->x + k->w && sy >= k->y && sy < k->y + k->h) return i;
    }
    return -1;
}

static int osk_mods(void) {
    return (osk_shift != MOD_OFF ? KMOD_SHIFT : 0) | (osk_ctrl != MOD_OFF ? KMOD_CTRL : 0)
         | (osk_alt != MOD_OFF ? KMOD_ALT : 0);
}

static void osk_draw_key(int idx, int pressed) {
    const OskKey *k = &osk_keys[idx];
    int latched = k->kind == KK_SHIFT ? osk_shift : k->kind == KK_CTRL ? osk_ctrl
                : k->kind == KK_ALT ? osk_alt : MOD_OFF;
    int inv = (latched == MOD_LOCKED) || pressed;
    unsigned char bg = inv ? 0 : 255, fg = inv ? 255 : 0;
    for (int y = 0; y < k->h; y++)
        for (int x = 0; x < k->w; x++) {
            int edge = y < 2 || y >= k->h - 2 || x < 2 || x >= k->w - 2;
            unsigned char v = edge ? fg : bg;
            put_rgb(k->x + x, k->y + y, v, v, v);
        }
    if (latched == MOD_ONESHOT)   // one-shot latch marker: thick inner top bar
        for (int y = 2; y < 10; y++)
            for (int x = 2; x < k->w - 2; x++) put_rgb(k->x + x, k->y + y, fg, fg, fg);
    char label[8];
    if (k->kind == KK_CHAR) {
        label[0] = osk_shift != MOD_OFF ? k->shifted : k->ch;
        label[1] = 0;
    } else {
        snprintf(label, sizeof label, "%s", k->label);
    }
    int tw = (int)strlen(label) * FONT.w;
    draw_glyphs(k->x + (k->w > tw ? (k->w - tw) / 2 : 0), k->y + (k->h - FONT.h) / 2, label, fg);
}

static void osk_swap_key(int idx) {
    const OskKey *k = &osk_keys[idx];
    quill_swap_ex(k->x, k->y, k->w, k->h, QUILL_MODE_COLOR4, 0, QUILL_CONTENT_COLOR);
}

// Draw the whole band into the FB; swap!=0 also pushes it to the panel.
static void osk_draw_all(int swap) {
    for (int y = osk_y0; y < H; y++)
        for (int x = 0; x < W; x++) put_rgb(x, y, 255, 255, 255);
    for (int i = 0; i < osk_nkeys; i++)
        osk_draw_key(i, i == osk_touch_pressed || i == osk_pen_pressed);
    if (swap) quill_swap_ex(0, osk_y0, W, OSK_H, QUILL_MODE_COLOR4, 0, QUILL_CONTENT_COLOR);
}

static void osk_send_vp(void) {
    char line[32];
    snprintf(line, sizeof line, "VP %d %d\n", W, osk_visible ? osk_y0 : H);
    send_line(line);
}

// ---------- URL bar: a one-line field drawn just above the band ----------
// While open, character and backspace/enter/esc keys edit a local buffer
// instead of going to the page; enter sends "NAV <url>" (https:// assumed).

#define URLBAR_H 72
static int url_mode = 0, url_len = 0, url_prefill = 0;
static char url_buf[1032];   // 1023-char URL + "\n" from the driver + slack

static void urlbar_draw(int swap) {
    int y0 = osk_y0 - URLBAR_H;
    for (int y = 0; y < URLBAR_H; y++)
        for (int x = 0; x < W; x++) {
            int edge = y < 3 || y >= URLBAR_H - 3 || x < 3 || x >= W - 3;
            unsigned char v = edge ? 0 : 255;
            put_rgb(x, y0 + y, v, v, v);
        }
    // Show the tail when the URL outgrows the field; trailing "_" is the caret.
    int maxc = (W - 48) / FONT.w - 1;
    const char *p = url_len > maxc ? url_buf + (url_len - maxc) : url_buf;
    int ty = y0 + (URLBAR_H - FONT.h) / 2;
    draw_glyphs(24, ty, p, 0);
    draw_glyphs(24 + (int)strlen(p) * FONT.w, ty, "_", 0);
    if (swap) quill_swap_ex(0, y0, W, URLBAR_H, QUILL_MODE_COLOR4, 0, QUILL_CONTENT_COLOR);
}

static void urlbar_close(int repaint) {
    url_mode = 0; url_len = 0; url_prefill = 0; url_buf[0] = 0;
    if (repaint && g_img_path) display_file(g_img_path, g_img_mode, 0);
}

static void urlbar_open(void) {
    url_mode = 1; url_len = 0; url_prefill = 0; url_buf[0] = 0;
    // Pre-fill with the driver-reported page URL (out/current.url, published
    // on every top-frame navigation); the first typed character replaces it,
    // and a bare enter re-navigates (poor man's reload).
    if (g_img_path) {
        char path[512];
        snprintf(path, sizeof path, "%s", g_img_path);
        char *slash = strrchr(path, '/');
        if (slash && (size_t)(slash + 1 - path) + sizeof "current.url" <= sizeof path) {
            strcpy(slash + 1, "current.url");
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(url_buf, sizeof url_buf, f) && strchr(url_buf, '\n')) {
                    // No newline = the URL outgrew the buffer; a truncated
                    // prefill would make bare-enter navigate somewhere broken.
                    url_buf[strcspn(url_buf, "\r\n")] = 0;
                    url_len = (int)strlen(url_buf);
                    url_prefill = url_len > 0;
                } else {
                    url_buf[0] = 0; url_len = 0;
                }
                fclose(f);
            }
        }
    }
    urlbar_draw(1);
}

static void urlbar_char(char c) {
    if (c == ' ') return;   // URLs have no spaces; must not count as "typing"
    if (url_prefill) { url_len = 0; url_buf[0] = 0; url_prefill = 0; }
    if (url_len < (int)sizeof url_buf - 1) { url_buf[url_len++] = c; url_buf[url_len] = 0; }
    urlbar_draw(1);
}

static void urlbar_named(const char *name) {
    if (!strcmp(name, "backspace")) {
        url_prefill = 0;
        if (url_len > 0) url_buf[--url_len] = 0;
        urlbar_draw(1);
    } else if (!strcmp(name, "esc")) {
        urlbar_close(1);
    } else if (!strcmp(name, "enter")) {
        if (url_len == 0) { urlbar_close(1); return; }
        char line[1100];
        snprintf(line, sizeof line, strstr(url_buf, "://") ? "NAV %s\n" : "NAV https://%s\n", url_buf);
        // URLs can embed credentials/tokens: log the fact, not the content.
        fprintf(stderr, "chromium_viewer: nav (%d chars)\n", url_len);
        send_line(line);
        urlbar_close(1);
    }
    // arrows/tab/del etc. are meaningless here: ignored
}

// The OSK claims the band, plus the URL bar's strip while it is open — a tap
// on the visible field must never click through to the page hidden under it.
static int osk_claims(int sy) {
    return osk_visible && sy >= (url_mode ? osk_y0 - URLBAR_H : osk_y0);
}

static void osk_show(void) {
    if (osk_visible || !FONT.glyphs) return;
    osk_visible = 1;
    osk_send_vp();
    osk_uncursor();
    osk_draw_all(1);
}

static void osk_hide(void) {
    if (!osk_visible) return;
    if (url_mode) urlbar_close(0);   // display_file below repaints the bar away
    osk_visible = 0;
    osk_touch_pressed = osk_pen_pressed = -1;
    osk_send_vp();
    if (g_img_path) display_file(g_img_path, g_img_mode, 0);
}

// One-shot modifiers disengage after the next real key; returns 1 if any did.
static int osk_consume_oneshots(void) {
    int changed = 0;
    if (osk_shift == MOD_ONESHOT) { osk_shift = MOD_OFF; changed = 1; }
    if (osk_ctrl == MOD_ONESHOT) { osk_ctrl = MOD_OFF; changed = 1; }
    if (osk_alt == MOD_ONESHOT) { osk_alt = MOD_OFF; changed = 1; }
    return changed;
}

static void osk_press(int sx, int sy, int *slot) {
    int idx = osk_hit(sx, sy);
    if (idx < 0) return;
    *slot = idx;
    osk_uncursor();   // the hover ring's save-under must not outlive this repaint
    OskKey *k = &osk_keys[idx];
    char line[48];
    // URL mode captures typing keys for the local buffer; modifier, hide,
    // rfsh, url and back keys keep their meaning.
    if (url_mode && (k->kind == KK_CHAR || k->kind == KK_NAMED)) {
        if (k->kind == KK_CHAR) urlbar_char(osk_shift != MOD_OFF ? k->shifted : k->ch);
        else urlbar_named(k->name);
        osk_draw_key(idx, 1);
        osk_swap_key(idx);
        if (osk_consume_oneshots()) osk_draw_all(1);
        return;
    }
    switch (k->kind) {
    case KK_SHIFT: case KK_CTRL: case KK_ALT: {
        int *m = k->kind == KK_SHIFT ? &osk_shift : k->kind == KK_CTRL ? &osk_ctrl : &osk_alt;
        *m = *m == MOD_OFF ? MOD_ONESHOT : *m == MOD_ONESHOT ? MOD_LOCKED : MOD_OFF;
        if (k->kind == KK_SHIFT) osk_draw_all(1);   // every char label changes
        else { osk_draw_key(idx, 1); osk_swap_key(idx); }
        return; }
    case KK_RFSH:
        if (g_img_path) display_file(g_img_path, g_img_mode, 1);
        return;
    case KK_HIDE:
        osk_hide();
        return;
    case KK_URL:
        if (url_mode) urlbar_close(1);
        else urlbar_open();
        osk_draw_key(idx, 1);
        osk_swap_key(idx);
        return;
    case KK_BACK:
        if (url_mode) urlbar_close(1);
        send_line("BACK\n");
        break;
    case KK_CHAR:
        snprintf(line, sizeof line, "KEY %d %d\n",
                 (int)(unsigned char)(osk_shift != MOD_OFF ? k->shifted : k->ch), osk_mods());
        send_line(line);
        break;
    case KK_NAMED:
        snprintf(line, sizeof line, "KEYN %s %d\n", k->name, osk_mods());
        send_line(line);
        break;
    }
    osk_draw_key(idx, 1);
    osk_swap_key(idx);
    if (osk_consume_oneshots()) osk_draw_all(1);
}

static void osk_release(int *slot) {
    if (*slot < 0) return;
    int idx = *slot;
    *slot = -1;
    if (!osk_visible) return;
    osk_uncursor();
    osk_draw_key(idx, idx == osk_touch_pressed || idx == osk_pen_pressed);
    osk_swap_key(idx);
}

static void clear_white(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) put_rgb(x, y, 255, 255, 255);
}

static int open_input(const char *needle) {
    char path[64], name[128], lower[128];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof path, "/sys/class/input/event%d/device/name", i);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (!fgets(name, sizeof name, f)) { fclose(f); continue; }
        fclose(f); memset(lower, 0, sizeof lower);
        for (size_t j = 0; j < sizeof lower - 1 && name[j]; j++) lower[j] = (name[j] >= 'A' && name[j] <= 'Z') ? name[j] + 32 : name[j];
        if (!strstr(lower, needle)) continue;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        int one = 1; ioctl(fd, EVIOCGRAB, &one);
        fprintf(stderr, "chromium_viewer: %s -> %s\n", needle, path);
        return fd;
    }
    return -1;
}

static void to_screen(int raw_x, int raw_y, int *sx, int *sy) {
    *sx = (TOUCH_MAX_X > 0) ? (int)((int64_t)raw_x * (W - 1) / TOUCH_MAX_X) : 0;
    *sy = (TOUCH_MAX_Y > 0) ? (int)((int64_t)raw_y * (H - 1) / TOUCH_MAX_Y) : 0;
    if (FLIP_X) *sx = W - 1 - *sx;
    if (FLIP_Y) *sy = H - 1 - *sy;
}

static int in_kbd_corner(int, int) {
    return 0;
}

// Pen state: hover position (screen px), tip contact, gesture start.
static int pen_x = -1, pen_y = -1, pen_touching = 0, pen_present = 0;
static int pen_down_x, pen_down_y;
static int pen_dirty = 0;
static int pen_on_kb = 0;   // this contact began on the keyboard band

// Erase the hover ring before any keyboard repaint: its save-under rect
// would otherwise be blitted back over the fresh key pixels. The main loop
// redraws the ring (over the new pixels) on the next pass.
static void osk_uncursor(void) {
    if (cur_x < 0) return;
    cursor_restore();
    if (pen_present) pen_dirty = 1;
}

// Page taps/swipes must never map into the hidden part of a shrunken
// viewport (or under the URL bar): clamp forwarded coordinates to the
// visible page area.
static int clamp_page_y(int sy) {
    if (!osk_visible) return sy;
    int lim = url_mode ? osk_y0 - URLBAR_H : osk_y0;
    return sy >= lim ? lim - 1 : sy;
}

static void drain_pen(int pen_fd) {
    struct input_event evs[64];
    static int raw_x = 0, raw_y = 0, touching = 0;
    if (pen_fd < 0) return;
    ssize_t n;
    while ((n = read(pen_fd, evs, sizeof evs)) > 0)
        for (int i = 0; i < (int)(n / sizeof(struct input_event)); i++) {
            if (evs[i].type == EV_ABS && evs[i].code == ABS_X) raw_x = evs[i].value;
            else if (evs[i].type == EV_ABS && evs[i].code == ABS_Y) raw_y = evs[i].value;
            else if (evs[i].type == EV_KEY && (evs[i].code == BTN_TOOL_PEN || evs[i].code == BTN_TOOL_RUBBER))
                pen_present = evs[i].value;
            else if (evs[i].type == EV_KEY && evs[i].code == BTN_TOUCH) touching = evs[i].value;
            else if (evs[i].type == EV_SYN) {
                int sx = (int)((int64_t)raw_x * (W - 1) / DIGI_MAX_X);
                int sy = (int)((int64_t)raw_y * (H - 1) / DIGI_MAX_Y);
                if (touching && !pen_touching) {
                    pen_down_x = sx; pen_down_y = sy;
                    if (osk_claims(sy)) { osk_press(sx, sy, &osk_pen_pressed); pen_on_kb = 1; }
                }
                if (!touching && pen_touching) {
                    // tip lifted: a key, a click, or a drag-scroll
                    if (pen_on_kb) { osk_release(&osk_pen_pressed); pen_on_kb = 0; }
                    else {
                        int dx = sx - pen_down_x, dy = sy - pen_down_y;
                        char line[64];
                        if (dx*dx + dy*dy <= TAP_MAX_DIST*TAP_MAX_DIST)
                            snprintf(line, sizeof line, "TAP %d %d\n", sx, clamp_page_y(sy));
                        else
                            snprintf(line, sizeof line, "SWIPE %d %d %d %d\n", pen_down_x,
                                     clamp_page_y(pen_down_y), sx, clamp_page_y(sy));
                        fprintf(stderr, "chromium_viewer: pen %s", line);
                        send_line(line);
                    }
                }
                pen_touching = touching;
                if (sx != pen_x || sy != pen_y) { pen_x = sx; pen_y = sy; pen_dirty = 1; }
            }
        }
}

// Gesture state for slot 0. Exactly two fingers have app-level gestures;
// three or four are ignored, and five exits.
static int g_down_x, g_down_y, g_last_x, g_last_y, g_tracking = 0, g_multi = 0;
static int g_peak_fingers = 0;
static int g_seeded = 0;         // first position of this contact captured yet?
static int g_on_kb = 0;          // slot-0 contact began on the keyboard band
static int g_pending_press = 0;  // key decision deferred to the report's SYN

static void drain_inputs(int pwr_fd, int touch_fd) {
    struct input_event evs[64];
    if (pwr_fd >= 0) { ssize_t n; while ((n = read(pwr_fd, evs, sizeof evs)) > 0)
        for (int i = 0; i < (int)(n / sizeof(struct input_event)); i++)
            if (evs[i].type == EV_KEY && evs[i].code == KEY_POWER && evs[i].value == 1) g_quit = 1; }
    static int slot_active[MAX_SLOTS] = {0}; static int cur_slot = 0;
    static int slot_x[MAX_SLOTS] = {0}, slot_y[MAX_SLOTS] = {0};
    if (touch_fd >= 0) { ssize_t n; while ((n = read(touch_fd, evs, sizeof evs)) > 0)
        for (int i = 0; i < (int)(n / sizeof(struct input_event)); i++) {
            // The SYN_REPORT closing the contact's first frame seals the down
            // point and decides key presses: slot state is authoritative there
            // even when the kernel dedup-filters the position events (a repeat
            // tap at the same spot delivers TRACKING_ID with no X/Y at all).
            // SYN_DROPPED (code 3) is not a frame boundary — never act on it.
            if (evs[i].type == EV_SYN) {
                if (evs[i].code == 0 && g_pending_press) {
                    g_pending_press = 0;
                    g_down_x = g_last_x = slot_x[0];
                    g_down_y = g_last_y = slot_y[0];
                    g_seeded = 1;   // later frames must not rewrite the down point
                    if (osk_visible && g_tracking && !g_multi) {
                        int sx, sy;
                        to_screen(slot_x[0], slot_y[0], &sx, &sy);
                        if (osk_claims(sy)) { osk_press(sx, sy, &osk_touch_pressed); g_on_kb = 1; }
                    }
                }
                continue;
            }
            if (evs[i].type != EV_ABS) continue;
            switch (evs[i].code) {
            case ABS_MT_SLOT:
                cur_slot = evs[i].value < 0 ? 0 : (evs[i].value >= MAX_SLOTS ? MAX_SLOTS-1 : evs[i].value);
                break;
            case ABS_MT_POSITION_X:
                slot_x[cur_slot] = evs[i].value;
                if (cur_slot == 0 && g_tracking) {
                    g_last_x = evs[i].value;
                    if (!g_seeded) g_down_x = evs[i].value;
                }
                break;
            case ABS_MT_POSITION_Y:
                slot_y[cur_slot] = evs[i].value;
                if (cur_slot == 0 && g_tracking) {
                    g_last_y = evs[i].value;
                    if (!g_seeded) { g_down_y = evs[i].value; g_seeded = 1; }
                }
                break;
            case ABS_MT_TRACKING_ID: {
                int was_down = slot_active[cur_slot];
                slot_active[cur_slot] = evs[i].value != -1;
                int fingers = 0; for (int s = 0; s < MAX_SLOTS; s++) fingers += slot_active[s];
                if (fingers >= 5) g_quit = 1;
                if (fingers >= 2) g_multi = 1;
                if (fingers > g_peak_fingers) g_peak_fingers = fingers;
                if (cur_slot == 0) {
                    if (!was_down && evs[i].value != -1) {
                        // finger down: the contact's own X/Y follow in this
                        // frame and overwrite this stale fallback seed —
                        // unless the kernel deduped them, which is why the
                        // key decision waits for SYN instead of X/Y.
                        g_tracking = 1; g_multi = (fingers >= 2); g_peak_fingers = fingers; g_seeded = 0;
                        g_down_x = g_last_x = slot_x[0];
                        g_down_y = g_last_y = slot_y[0];
                        g_pending_press = 1;
                    } else if (was_down && evs[i].value == -1 && g_tracking) {
                        g_tracking = 0;
                        if (g_on_kb) { osk_release(&osk_touch_pressed); g_on_kb = 0; g_multi = 0; g_peak_fingers = 0; break; }
                        int dsx, dsy, usx, usy;
                        to_screen(g_down_x, g_down_y, &dsx, &dsy);
                        to_screen(g_last_x, g_last_y, &usx, &usy);
                        int dx = usx - dsx, dy = usy - dsy;
                        if (g_multi) {
                            if (g_peak_fingers == 2 && abs(dy) >= TWO_FINGER_MIN_DIST && abs(dy) > abs(dx)) {
                                if (dy < 0) {
                                    fprintf(stderr, "chromium_viewer: two-finger up -> keyboard\n");
                                    osk_show();
                                } else if (g_img_path) {
                                    fprintf(stderr, "chromium_viewer: two-finger down -> full refresh\n");
                                    display_file(g_img_path, g_img_mode, 1);
                                }
                            }
                        } else {
                            char line[64];
                            if (dx*dx + dy*dy <= TAP_MAX_DIST*TAP_MAX_DIST) {
                                snprintf(line, sizeof line, "TAP %d %d\n", usx, clamp_page_y(usy));
                            } else {
                                snprintf(line, sizeof line, "SWIPE %d %d %d %d\n", dsx,
                                         clamp_page_y(dsy), usx, clamp_page_y(usy));
                            }
                            fprintf(stderr, "chromium_viewer: touch %s", line);
                            send_line(line);
                        }
                        g_multi = 0;
                        g_peak_fingers = 0;
                    }
                }
                break; }
            }
        }}
}

static void blit_image(const QImage &src, const char *mode) {
    QImage img = src.convertToFormat(QImage::Format_RGB32);
    int dw = W, dh = H, dx0 = 0, dy0 = 0;
    if (strcmp(mode, "stretch") != 0) {
        double sc = (strcmp(mode, "fill") == 0)
            ? qMax((double)W / img.width(), (double)H / img.height())
            : qMin((double)W / img.width(), (double)H / img.height());
        dw = (int)(img.width() * sc); dh = (int)(img.height() * sc);
        dx0 = (W - dw) / 2; dy0 = (H - dh) / 2;
    }
    QImage scaled = img.scaled(dw, dh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    clear_white();
    int cw = qMin(dw, W - dx0), ch = qMin(dh, H - dy0);
    int sx0 = 0;
    for (int y = 0; y < ch; y++) {
        const QRgb *row = (const QRgb *)scaled.constScanLine(y);
        for (int x = 0; x < cw; x++) {
            QRgb px = row[sx0 + x];
            put_rgb(dx0 + x, dy0 + y, qRed(px), qGreen(px), qBlue(px));
        }
    }
}

// Load + display the image; full=1 does a cleaning full refresh.
static int display_file(const char *path, const char *mode, int full) {
    QImage img(path);
    if (img.isNull()) { fprintf(stderr, "chromium_viewer: could not load %s\n", path); return -1; }
    cur_x = cur_y = -1;   // saved under-cursor pixels belong to the old frame
    if (pen_present) pen_dirty = 1;   // repaint erases the ring: redraw it after
    if (osk_visible) {
        // Keyboard up: the driver's viewport matches the page area, so blit
        // 1:1 anchored top-left (taps stay an identity map) and keep the band.
        QImage rgb = img.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < osk_y0; y++) {
            const QRgb *row = y < rgb.height() ? (const QRgb *)rgb.constScanLine(y) : NULL;
            for (int x = 0; x < W; x++) {
                if (row && x < rgb.width()) {
                    QRgb px = row[x];
                    put_rgb(x, y, qRed(px), qGreen(px), qBlue(px));
                } else {
                    put_rgb(x, y, 255, 255, 255);
                }
            }
        }
        osk_draw_all(0);
        if (url_mode) urlbar_draw(0);   // keep the field on top of fresh frames
    } else {
        blit_image(img, mode);
    }
    quill_swap_ex(0, 0, W, H, QUILL_MODE_COLOR4, full, QUILL_CONTENT_COLOR);
    quill_process_events();
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);   // dead FIFO reader must not kill us
    if (argc < 2) { fprintf(stderr, "usage: chromium_viewer /path/to/page.png [fit|fill|stretch] [full] [fifo]\n"); return 2; }
    const char *mode = argc >= 3 ? argv[2] : "fit";
    int full = argc >= 4 ? atoi(argv[3]) : 0;
    const char *fifo_path = argc >= 5 ? argv[4] : NULL;

    const char *flip = getenv("CHROMIUM_TOUCH_FLIP");
    if (flip) { FLIP_X = strchr(flip, 'x') != NULL; FLIP_Y = strchr(flip, 'y') != NULL; }

    if (quill_init() != 0) return 1;
    W = quill_width(); H = quill_height(); STRIDE = quill_stride(); BPP = STRIDE / (W ? W : 1); FB = quill_buffer();
    fprintf(stderr, "chromium_viewer: %dx%d stride %d bpp %d fmt %d image=%s mode=%s full=%d fifo=%s flip=%d%d\n",
            W, H, STRIDE, BPP, quill_format(), argv[1], mode, full, fifo_path ? fifo_path : "-", FLIP_X, FLIP_Y);

    g_img_path = argv[1]; g_img_mode = mode;
    if (font_init() != 0) fprintf(stderr, "chromium_viewer: bad embedded font, keyboard disabled\n");
    else osk_init();

    struct stat st_prev;
    int have_stat = (stat(argv[1], &st_prev) == 0);

    if (display_file(argv[1], mode, full) != 0) return 1;

    if (fifo_path) {
        FIFO_FD = open(fifo_path, O_WRONLY | O_NONBLOCK);
        fprintf(stderr, "chromium_viewer: fifo %s\n", FIFO_FD >= 0 ? "connected" : "not ready yet (will retry)");
        if (FIFO_FD >= 0 && FONT.glyphs) osk_send_vp();   // sync viewport to keyboard state
    }

    int pwr_fd = open_input("powerkey"), touch_fd = open_input("touch");
    int pen_fd = open_input("marker");   // "Elan marker input" — the pen digitizer
    if (touch_fd >= 0) {
        struct input_absinfo ax, ay;
        if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0) TOUCH_MAX_X = ax.maximum;
        if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0) TOUCH_MAX_Y = ay.maximum;
        fprintf(stderr, "chromium_viewer: touch range %dx%d\n", TOUCH_MAX_X, TOUCH_MAX_Y);
    }

    int inotify_fd = -1, inotify_wd = -1;
    {
        char dirbuf[PATH_MAX];
        snprintf(dirbuf, sizeof dirbuf, "%s", argv[1]);
        const char *dir = dirname(dirbuf);
        inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd >= 0) {
            inotify_wd = inotify_add_watch(inotify_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
            if (inotify_wd < 0) { close(inotify_fd); inotify_fd = -1; }
        }
        if (inotify_fd < 0) fprintf(stderr, "chromium_viewer: inotify unavailable, falling back to poll\n");
    }

    int poll_n = 0;

    struct pollfd pfds[4] = {{.fd = pwr_fd, .events = POLLIN, .revents = 0},
                             {.fd = touch_fd, .events = POLLIN, .revents = 0},
                             {.fd = pen_fd, .events = POLLIN, .revents = 0},
                             {.fd = inotify_fd, .events = POLLIN, .revents = 0}};
    const char *base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    while (!g_quit) {
        int timeout = pen_present ? 30 : (inotify_fd >= 0 ? 200 : 100);
        poll(pfds, inotify_fd >= 0 ? 4 : 3, timeout);
        drain_inputs(pwr_fd, touch_fd);
        drain_pen(pen_fd);
        // Live pen cursor: erase the old ring, draw at the new hover position.
        if (pen_dirty) {
            pen_dirty = 0;
            cursor_restore();
            if (pen_present) cursor_draw(pen_x, pen_y);
        } else if (!pen_present && cur_x >= 0) {
            cursor_restore();   // pen left the glass: clear the ring
        }
        quill_process_events();
        int need_repaint = 0;
        if (inotify_fd >= 0 && (pfds[3].revents & POLLIN)) {
            char buf[4096];
            ssize_t len;
            while ((len = read(inotify_fd, buf, sizeof buf)) > 0) {
                for (char *p = buf; p < buf + len; ) {
                    struct inotify_event *ev = (struct inotify_event *)p;
                    if (ev->len > 0 && strcmp(ev->name, base) == 0) need_repaint = 1;
                    else if (ev->len == 0) need_repaint = 1;
                    p += sizeof(struct inotify_event) + ev->len;
                }
            }
        } else if (inotify_fd < 0) {
            if (++poll_n >= 2) {   // ~5 Hz stat fallback when inotify unavailable
                poll_n = 0;
                struct stat st;
                if (stat(argv[1], &st) == 0) {
                    if (!have_stat || st.st_mtim.tv_sec != st_prev.st_mtim.tv_sec
                        || st.st_mtim.tv_nsec != st_prev.st_mtim.tv_nsec
                        || st.st_ino != st_prev.st_ino || st.st_size != st_prev.st_size) {
                        st_prev = st; have_stat = 1;
                        need_repaint = 1;
                    }
                }
            }
        }
        if (need_repaint) {
            struct stat st;
            if (stat(argv[1], &st) == 0) { st_prev = st; have_stat = 1; }
            display_file(argv[1], mode, 0);   // partial refresh for updates
        }
        // Retry the FIFO if the reader appeared after us (or a write failed).
        // VP is stateful, so every (re)connect resyncs the driver's viewport
        // to the current keyboard state — a VP sent while disconnected is
        // otherwise lost and the composer stays hidden behind the band.
        if (fifo_path && FIFO_FD < 0) {
            static int retry = 0;
            if (++retry >= 20) {
                retry = 0;
                FIFO_FD = open(fifo_path, O_WRONLY | O_NONBLOCK);
                if (FIFO_FD >= 0) {
                    fprintf(stderr, "chromium_viewer: fifo connected\n");
                    if (FONT.glyphs) osk_send_vp();
                }
            }
        }
    }
    return 0;
}
