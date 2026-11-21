// HDMI text console: 640x480x32 framebuffer from the firmware, 80x30 cells of
// 8x16. Mirrors everything written to the UART (uart_putc calls fbcon_putc),
// with the subset of VT100/ANSI that PiOS programs use: CR LF BS, ESC[ n A B C
// D H J K m (0, 7) and ?25 h/l (cursor show/hide).
// shortcut: scrolling copies the whole screen through uncached memory (~1 MB);
// use a virtual framebuffer twice as tall and pan with tag 0x48009 if it is slow.
#include "kernel.h"
#include "mailbox.h"
#include "mm.h"

#define W 640
#define H 480
#define COLS (W / 8)
#define ROWS (H / 16)
#define FG 0x00c0c0c0u
#define BG 0x00000000u

extern const uint8_t font8x16[95][16];

static volatile uint32_t *fb;
static uint32_t pitch;          // in pixels
static int cx, cy, inverse, cursor_on = 1;
static int esc;                 // 0 none, 1 after ESC, 2 in CSI
static int par[4], npar, priv;

static void cell(int col, int row, char c)
{
    const uint8_t *g = c >= 32 && c <= 126 ? font8x16[c - 32] : font8x16['?' - 32];
    uint32_t fg = inverse ? BG : FG, bg = inverse ? FG : BG;
    volatile uint32_t *p = fb + (uint32_t)row * 16 * pitch + (uint32_t)col * 8;
    for (int y = 0; y < 16; y++, p += pitch)
        for (int x = 0; x < 8; x++)
            p[x] = (g[y] & (0x80 >> x)) ? fg : bg;
}

static void clear_cells(int row, int col0, int col1)    // [col0, col1) on one row
{
    for (int y = 0; y < 16; y++) {
        volatile uint32_t *p = fb + (uint32_t)(row * 16 + y) * pitch;
        for (int x = col0 * 8; x < col1 * 8; x++)
            p[x] = BG;
    }
}

static void cursor_xor(void)
{
    if (!cursor_on || cx >= COLS)
        return;
    volatile uint32_t *p = fb + (uint32_t)cy * 16 * pitch + (uint32_t)cx * 8;
    for (int y = 0; y < 16; y++, p += pitch)
        for (int x = 0; x < 8; x++)
            p[x] ^= FG;
}

static void newline(void)
{
    cx = 0;
    if (++cy < ROWS)
        return;
    cy = ROWS - 1;
    for (uint32_t y = 0; y < (ROWS - 1) * 16; y++) {
        volatile uint32_t *d = fb + y * pitch, *s = d + 16 * pitch;
        for (int x = 0; x < W; x++)
            d[x] = s[x];
    }
    clear_cells(ROWS - 1, 0, COLS);
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void csi(char f)
{
    int n = npar && par[0] ? par[0] : 1;
    switch (f) {
    case 'A': cy = clamp(cy - n, 0, ROWS - 1); break;
    case 'B': cy = clamp(cy + n, 0, ROWS - 1); break;
    case 'C': cx = clamp(cx + n, 0, COLS - 1); break;
    case 'D': cx = clamp(cx - n, 0, COLS - 1); break;
    case 'H':
        cy = clamp((npar > 0 && par[0] ? par[0] : 1) - 1, 0, ROWS - 1);
        cx = clamp((npar > 1 && par[1] ? par[1] : 1) - 1, 0, COLS - 1);
        break;
    case 'J':
        if (npar && par[0] == 2)
            for (int r = 0; r < ROWS; r++)
                clear_cells(r, 0, COLS);
        break;
    case 'K':
        if (cx < COLS)
            clear_cells(cy, cx, COLS);
        break;
    case 'm':
        for (int i = 0; i < (npar ? npar : 1); i++)
            inverse = par[i] == 7 ? 1 : par[i] == 0 ? 0 : inverse;
        break;
    case 'h': case 'l':
        if (priv && par[0] == 25)
            cursor_on = f == 'h';
        break;
    }
}

static void put(char c)
{
    if (esc == 1) {
        esc = c == '[' ? 2 : 0;
        npar = priv = 0;
        par[0] = 0;
        return;
    }
    if (esc == 2) {
        if (c == '?') {
            priv = 1;
        } else if (c >= '0' && c <= '9') {
            if (!npar)
                npar = 1;
            par[npar - 1] = par[npar - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (!npar)
                npar = 1;
            if (npar < 4)
                par[npar++] = 0;
        } else {
            csi(c);
            esc = 0;
        }
        return;
    }
    switch (c) {
    case 033: esc = 1; break;
    case '\r': cx = 0; break;
    case '\n': newline(); break;
    case '\b': if (cx > 0) cx--; break;
    default:
        if (c < 32)
            break;
        if (cx >= COLS)             // wrap at the next printable, like a VT100
            newline();
        cell(cx++, cy, c);
    }
}

void fbcon_putc(char c)
{
    if (!fb)
        return;
    uint64_t daif = read_sysreg(daif);
    __asm__ volatile("msr daifset, #2");    // UART echo from IRQ also prints
    cursor_xor();
    put(c);
    cursor_xor();
    write_sysreg(daif, daif);
}

void fbcon_init(void)
{
    uint32_t pa, size, p;
    if (mbox_framebuffer(W, H, &pa, &size, &p) != 0 || size < p * H) {
        kprintf("[FB] no framebuffer (HDMI console off)\n");
        return;
    }
    mmu_map_nc(pa, size);
    pitch = p / 4;
    fb = (volatile uint32_t *)(uintptr_t)pa;
    for (int r = 0; r < ROWS; r++)
        clear_cells(r, 0, COLS);
    cursor_xor();
    kprintf("[FB] %dx%d console at %x (%d x %d text)\n", W, H, pa, COLS, ROWS);
}
