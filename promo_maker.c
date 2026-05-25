/*
 * Image date-stamping CLI.
 *
 * Loads a PNG, renders two lines of text using a TTF/OTF font, writes a new
 * PNG. Line 1 is "MON. MM-DD-YY" computed as the upcoming 2nd Monday of the
 * month (this month's if not yet passed, else next month's). Line 2 defaults
 * to "4:30-6PM" and is configurable. The input file is never modified.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Third-party headers: suppress warnings we don't control. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#pragma GCC diagnostic pop

#define ANSI_RESET   "\x1b[0m"
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_BOLD    "\x1b[1m"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_IMG_DIM   16384
#define MAX_FONT_SIZE (32 * 1024 * 1024)
#define MAX_PIXEL_PX  4096
#define DEFAULT_TIME  "4:30-6PM"
#define DEFAULT_COLOR 0xFFFFFFu

typedef struct {
    const char *input;
    const char *font;
    const char *output_dir;
    const char *out_name;  /* optional override; NULL = auto-derive */
    const char *time_text;
    long        x;
    long        y;
    long        size;
    uint32_t    color;     /* 0x00RRGGBB */
    int         force;
    int         no_cleanup;
    int         have_x;
    int         have_y;
    int         have_size;
    int         have_color;
} opts_t;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, ANSI_BOLD ANSI_RED "error: " ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void usage(FILE *out) {
    fprintf(out,
        ANSI_BOLD "Usage:" ANSI_RESET " test -i <png> -f <ttf> -o <dir> -y <n> -s <n> [opts]\n"
        "\n"
        "Stamps a date and time onto a PNG using a TTF/OTF font.\n"
        "Line 1: MON. MM-DD-YY (upcoming 2nd Monday of the month).\n"
        "Line 2: configurable, defaults to \"%s\".\n"
        "Each line is centered horizontally by default; pass -x to left-align.\n"
        "\n"
        ANSI_BOLD "Required:\n" ANSI_RESET
        "  -i, --input  <path>    base PNG (read-only)\n"
        "  -f, --font   <path>    TTF or OTF font\n"
        "  -o, --output <dir>     output directory (must exist)\n"
        "  -y <int>               top-left Y of text block, in pixels\n"
        "  -s, --size   <int>     font pixel height (1..%d)\n"
        "\n"
        ANSI_BOLD "Optional:\n" ANSI_RESET
        "  -x <int>               left-align text at this X (default: center each line)\n"
        "  -n, --name   <file>    output filename (default <input-stem>_YYYY-MM-DD.png);\n"
        "                         \".png\" appended if missing; must not contain '/'\n"
        "  -c, --color  <RRGGBB>  hex text color (default FFFFFF)\n"
        "  -t, --time   <str>     line-2 text (default \"%s\")\n"
        "      --force            allow overwrite of output file\n"
        "      --no-cleanup       keep prior <stem>_YYYY-MM-DD.png files in output dir\n"
        "                         (default: delete them after a successful write)\n"
        "  -h, --help             this message\n",
        DEFAULT_TIME, MAX_PIXEL_PX, DEFAULT_TIME);
}

static long parse_long(const char *s, long lo, long hi, const char *name) {
    if (!s || !*s) die("--%s requires a value", name);
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        die("--%s: not a valid integer: \"%s\"", name, s);
    if (v < lo || v > hi)
        die("--%s: %ld out of range [%ld..%ld]", name, v, lo, hi);
    return v;
}

static uint32_t parse_color(const char *s) {
    if (!s) die("--color requires a value");
    size_t n = strlen(s);
    if (n != 6) die("--color: expected 6 hex digits (RRGGBB), got \"%s\"", s);
    uint32_t v = 0;
    for (size_t i = 0; i < 6; ++i) {
        char c = s[i];
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else { die("--color: bad hex digit '%c'", c); return 0; }
        v = (v << 4) | (uint32_t)d;
    }
    return v;
}

/* Match a long-option token like "--name" or "--name=value".
 * Returns 1 on match. On match, sets *value_inline to the part after '=' or
 * NULL if there was no '='. */
static int match_long(const char *tok, const char *name, const char **value_inline) {
    if (tok[0] != '-' || tok[1] != '-') return 0;
    const char *p = tok + 2;
    size_t nlen = strlen(name);
    if (strncmp(p, name, nlen) != 0) return 0;
    char after = p[nlen];
    if (after == '\0') { *value_inline = NULL; return 1; }
    if (after == '=')  { *value_inline = p + nlen + 1; return 1; }
    return 0;
}

/* Consume a value for the current option: either inline (--name=val) or the
 * next argv slot (--name val). Advances *i past the consumed slot. */
static const char *take_value(int argc, char **argv, int *i,
                              const char *inline_val, const char *name) {
    if (inline_val) return inline_val;
    if (*i + 1 >= argc) die("--%s requires a value", name);
    *i += 1;
    return argv[*i];
}

static void parse_args(int argc, char **argv, opts_t *o) {
    memset(o, 0, sizeof *o);
    o->time_text = DEFAULT_TIME;
    o->color     = DEFAULT_COLOR;

    if (argc < 2) {
        fprintf(stderr, ANSI_BOLD ANSI_RED "No arguments specified." ANSI_RESET "\n\n");
        usage(stderr);
        exit(1);
    }

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *vi = NULL;

        if (a[0] != '-' || a[1] == '\0')
            die("unexpected token \"%s\" (expected an option)", a);

        if (strcmp(a, "-h") == 0 || match_long(a, "help", &vi)) {
            usage(stdout);
            exit(0);
        }

        if (match_long(a, "input",  &vi)) { o->input      = take_value(argc, argv, &i, vi, "input"); continue; }
        if (match_long(a, "font",   &vi)) { o->font       = take_value(argc, argv, &i, vi, "font");  continue; }
        if (match_long(a, "output", &vi)) { o->output_dir = take_value(argc, argv, &i, vi, "output"); continue; }
        if (match_long(a, "size",   &vi)) {
            o->size = parse_long(take_value(argc, argv, &i, vi, "size"), 1, MAX_PIXEL_PX, "size");
            o->have_size = 1; continue;
        }
        if (match_long(a, "color",  &vi)) {
            o->color = parse_color(take_value(argc, argv, &i, vi, "color"));
            o->have_color = 1; continue;
        }
        if (match_long(a, "time",   &vi)) { o->time_text = take_value(argc, argv, &i, vi, "time");  continue; }
        if (match_long(a, "name",   &vi)) { o->out_name  = take_value(argc, argv, &i, vi, "name");  continue; }
        if (match_long(a, "force",  &vi)) {
            if (vi) die("--force does not take a value");
            o->force = 1; continue;
        }
        if (match_long(a, "no-cleanup", &vi)) {
            if (vi) die("--no-cleanup does not take a value");
            o->no_cleanup = 1; continue;
        }

        /* Short flags. Each takes the next argv slot as its value. */
        if (a[0] == '-' && a[1] != '-' && a[2] == '\0') {
            switch (a[1]) {
                case 'i': o->input      = take_value(argc, argv, &i, NULL, "input");  continue;
                case 'f': o->font       = take_value(argc, argv, &i, NULL, "font");   continue;
                case 'o': o->output_dir = take_value(argc, argv, &i, NULL, "output"); continue;
                case 'x':
                    o->x = parse_long(take_value(argc, argv, &i, NULL, "x"), 0, MAX_PIXEL_PX, "x");
                    o->have_x = 1; continue;
                case 'y':
                    o->y = parse_long(take_value(argc, argv, &i, NULL, "y"), 0, MAX_PIXEL_PX, "y");
                    o->have_y = 1; continue;
                case 's':
                    o->size = parse_long(take_value(argc, argv, &i, NULL, "size"), 1, MAX_PIXEL_PX, "size");
                    o->have_size = 1; continue;
                case 'c':
                    o->color = parse_color(take_value(argc, argv, &i, NULL, "color"));
                    o->have_color = 1; continue;
                case 't': o->time_text = take_value(argc, argv, &i, NULL, "time");   continue;
                case 'n': o->out_name  = take_value(argc, argv, &i, NULL, "name");   continue;
                default:  die("unknown option \"-%c\"", a[1]);
            }
        }

        die("unknown option \"%s\"", a);
    }

    if (!o->input)      die("missing required --input/-i");
    if (!o->font)       die("missing required --font/-f");
    if (!o->output_dir) die("missing required --output/-o");
    if (!o->have_y)     die("missing required -y");
    if (!o->have_size)  die("missing required --size/-s");

    for (size_t k = 0; k < 3; ++k) {
        const char *p = (k == 0) ? o->input : (k == 1) ? o->font : o->output_dir;
        if (strlen(p) >= PATH_MAX) die("path too long (>= %d bytes)", PATH_MAX);
    }
    if (strlen(o->time_text) >= 128) die("--time too long (>= 128 bytes)");
    if (o->out_name) {
        if (o->out_name[0] == '\0') die("--name must not be empty");
        if (strchr(o->out_name, '/')) die("--name must not contain '/'");
        if (strlen(o->out_name) >= 256) die("--name too long (>= 256 bytes)");
    }
}

/* Compute year/month/day of the upcoming 2nd Monday.
 * If this month's 2nd Monday has not yet passed, use it; else use next
 * month's. Outputs are: y_out = full year (e.g. 2026), m_out = 1..12,
 * d_out = day-of-month. */
static void compute_second_monday(int *y_out, int *m_out, int *d_out) {
    time_t now_t = time(NULL);
    if (now_t == (time_t)-1) die("time() failed");
    struct tm now;
    if (!localtime_r(&now_t, &now)) die("localtime_r failed");

    int year = now.tm_year;   /* years since 1900 */
    int mon  = now.tm_mon;    /* 0..11 */
    int today_mday = now.tm_mday;

    for (int attempt = 0; attempt < 2; ++attempt) {
        struct tm first = {0};
        first.tm_year = year;
        first.tm_mon  = mon;
        first.tm_mday = 1;
        first.tm_hour = 12;       /* noon avoids DST boundary glitches */
        first.tm_isdst = -1;
        if (mktime(&first) == (time_t)-1) die("mktime failed");

        int first_wday = first.tm_wday;             /* 0=Sun..6=Sat */
        int first_monday = 1 + ((1 - first_wday + 7) % 7);
        int second_monday = first_monday + 7;

        if (attempt == 1 || today_mday <= second_monday) {
            *y_out = first.tm_year + 1900;
            *m_out = first.tm_mon + 1;
            *d_out = second_monday;
            return;
        }
        /* Advance to next month; mktime will normalize year on rollover. */
        mon += 1;
        struct tm probe = {0};
        probe.tm_year = year;
        probe.tm_mon  = mon;
        probe.tm_mday = 1;
        probe.tm_hour = 12;
        probe.tm_isdst = -1;
        if (mktime(&probe) == (time_t)-1) die("mktime failed");
        year = probe.tm_year;
        mon  = probe.tm_mon;
        today_mday = 0;  /* belt-and-suspenders: 0 <= any second_monday */
    }
}

/* Returns malloc'd buffer; *out_size set; caller frees. */
static unsigned char *read_file(const char *path, size_t *out_size, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open \"%s\": %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); die("fseek failed: %s", strerror(errno)); }
    long n = ftell(f);
    if (n < 0) { fclose(f); die("ftell failed: %s", strerror(errno)); }
    if ((size_t)n > cap) { fclose(f); die("\"%s\" too large (%ld > %zu bytes)", path, n, cap); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); die("fseek failed: %s", strerror(errno)); }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); die("out of memory"); }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); die("short read on \"%s\"", path); }
    *out_size = (size_t)n;
    return buf;
}

/* Pen X advance after rendering `text` (returns total advance in px). */
static long measure_text(const stbtt_fontinfo *font, float scale, const char *text) {
    double pen = 0.0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        int aw, lsb;
        stbtt_GetCodepointHMetrics(font, *p, &aw, &lsb);
        pen += aw * (double)scale;
        if (p[1])
            pen += stbtt_GetCodepointKernAdvance(font, p[0], p[1]) * (double)scale;
    }
    return (long)(pen + 0.5);
}

/* Composite one alpha-only glyph bitmap onto the RGBA image. */
static void blit_glyph(unsigned char *img, int w, int h,
                       const unsigned char *glyph, int gw, int gh,
                       int dst_x, int dst_y, uint32_t color) {
    int sr = (int)((color >> 16) & 0xFF);
    int sg = (int)((color >>  8) & 0xFF);
    int sb = (int)( color        & 0xFF);
    for (int j = 0; j < gh; ++j) {
        int iy = dst_y + j;
        if (iy < 0 || iy >= h) continue;
        for (int k = 0; k < gw; ++k) {
            int ix = dst_x + k;
            if (ix < 0 || ix >= w) continue;
            int a = glyph[j * gw + k];
            if (a == 0) continue;
            unsigned char *px = img + (iy * w + ix) * 4;
            int inv = 255 - a;
            px[0] = (unsigned char)((sr * a + px[0] * inv + 127) / 255);
            px[1] = (unsigned char)((sg * a + px[1] * inv + 127) / 255);
            px[2] = (unsigned char)((sb * a + px[2] * inv + 127) / 255);
            if (px[3] < (unsigned char)a) px[3] = (unsigned char)a;
        }
    }
}

static void render_line(const stbtt_fontinfo *font, float scale,
                        unsigned char *img, int w, int h,
                        long pen_x, int baseline_y,
                        const char *text, uint32_t color) {
    double pen = (double)pen_x;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        int aw, lsb;
        stbtt_GetCodepointHMetrics(font, *p, &aw, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(font, *p, scale, scale, &x0, &y0, &x1, &y1);
        int gw, gh, gxoff, gyoff;
        unsigned char *bmp = stbtt_GetCodepointBitmap(
            font, scale, scale, *p, &gw, &gh, &gxoff, &gyoff);
        if (bmp) {
            int dst_x = (int)(pen + 0.5) + x0;
            int dst_y = baseline_y + y0;
            blit_glyph(img, w, h, bmp, gw, gh, dst_x, dst_y, color);
            stbtt_FreeBitmap(bmp, NULL);
        }
        pen += aw * (double)scale;
        if (p[1])
            pen += stbtt_GetCodepointKernAdvance(font, p[0], p[1]) * (double)scale;
    }
}

/* Extract basename (no directory) and strip a trailing ".png" / ".PNG". */
static void input_stem(const char *path, char *out, size_t cap) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n >= 4) {
        const char *ext = base + n - 4;
        if ((ext[0] == '.') &&
            (ext[1] == 'p' || ext[1] == 'P') &&
            (ext[2] == 'n' || ext[2] == 'N') &&
            (ext[3] == 'g' || ext[3] == 'G')) {
            n -= 4;
        }
    }
    if (n == 0) die("input filename has empty stem");
    if (n + 1 > cap) die("input basename too long");
    memcpy(out, base, n);
    out[n] = '\0';
}

static int path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static int is_directory(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Delete prior outputs in output_dir whose filename matches exactly
 * <stem>_YYYY-MM-DD.png (case-insensitive ".png"). Skips new_basename so we
 * never touch the file we just wrote. Refuses anything that isn't a regular
 * file via lstat -- no directories deleted, no symlinks followed. Errors are
 * non-fatal warnings; the new write has already succeeded. */
static void cleanup_prior_outputs(const char *output_dir,
                                  const char *stem,
                                  const char *new_basename) {
    DIR *d = opendir(output_dir);
    if (!d) {
        fprintf(stderr, ANSI_YELLOW "warning: " ANSI_RESET
                "cannot scan \"%s\": %s\n", output_dir, strerror(errno));
        return;
    }

    size_t stem_len = strlen(stem);
    size_t expected_len = stem_len + 15;  /* '_' + YYYY-MM-DD + ".png" */

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strlen(name) != expected_len) continue;
        if (memcmp(name, stem, stem_len) != 0) continue;
        if (name[stem_len] != '_') continue;

        const char *date = name + stem_len + 1;
        if (!isdigit((unsigned char)date[0]) ||
            !isdigit((unsigned char)date[1]) ||
            !isdigit((unsigned char)date[2]) ||
            !isdigit((unsigned char)date[3]) ||
            date[4] != '-' ||
            !isdigit((unsigned char)date[5]) ||
            !isdigit((unsigned char)date[6]) ||
            date[7] != '-' ||
            !isdigit((unsigned char)date[8]) ||
            !isdigit((unsigned char)date[9]))
            continue;

        const char *ext = name + stem_len + 11;
        if (ext[0] != '.') continue;
        if (ext[1] != 'p' && ext[1] != 'P') continue;
        if (ext[2] != 'n' && ext[2] != 'N') continue;
        if (ext[3] != 'g' && ext[3] != 'G') continue;

        if (strcmp(name, new_basename) == 0) continue;

        char full[PATH_MAX];
        int fn = snprintf(full, sizeof full, "%s/%s", output_dir, name);
        if (fn < 0 || (size_t)fn >= sizeof full) {
            fprintf(stderr, ANSI_YELLOW "warning: " ANSI_RESET
                    "path too long, skipping \"%s\"\n", name);
            continue;
        }

        struct stat st;
        if (lstat(full, &st) != 0) {
            fprintf(stderr, ANSI_YELLOW "warning: " ANSI_RESET
                    "cannot stat \"%s\": %s\n", full, strerror(errno));
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;

        if (unlink(full) != 0) {
            fprintf(stderr, ANSI_YELLOW "warning: " ANSI_RESET
                    "could not delete \"%s\": %s\n", full, strerror(errno));
            continue;
        }
        printf(ANSI_YELLOW "removed " ANSI_RESET "%s\n", full);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    opts_t opt;
    parse_args(argc, argv, &opt);

    if (!is_directory(opt.output_dir))
        die("--output \"%s\" is not an existing directory", opt.output_dir);

    int y_full, mon1, day;
    compute_second_monday(&y_full, &mon1, &day);

    char line1[32];
    int n1 = snprintf(line1, sizeof line1, "MON. %02d-%02d-%02d",
                      mon1, day, y_full % 100);
    if (n1 < 0 || (size_t)n1 >= sizeof line1) die("date format overflow");
    const char *line2 = opt.time_text;

    char stem[256];
    input_stem(opt.input, stem, sizeof stem);

    char out_path[PATH_MAX];
    int np;
    if (opt.out_name) {
        size_t nn = strlen(opt.out_name);
        int need_ext = !(nn >= 4 &&
                         (opt.out_name[nn-4] == '.') &&
                         (opt.out_name[nn-3] == 'p' || opt.out_name[nn-3] == 'P') &&
                         (opt.out_name[nn-2] == 'n' || opt.out_name[nn-2] == 'N') &&
                         (opt.out_name[nn-1] == 'g' || opt.out_name[nn-1] == 'G'));
        np = snprintf(out_path, sizeof out_path, "%s/%s%s",
                      opt.output_dir, opt.out_name, need_ext ? ".png" : "");
    } else {
        np = snprintf(out_path, sizeof out_path, "%s/%s_%04d-%02d-%02d.png",
                      opt.output_dir, stem, y_full, mon1, day);
    }
    if (np < 0 || (size_t)np >= sizeof out_path)
        die("output path too long");

    if (path_exists(out_path) && !opt.force)
        die("output \"%s\" exists; pass --force to overwrite", out_path);

    /* Load font. */
    size_t font_size = 0;
    unsigned char *font_buf = read_file(opt.font, &font_size, MAX_FONT_SIZE);
    stbtt_fontinfo font;
    int font_off = stbtt_GetFontOffsetForIndex(font_buf, 0);
    if (font_off < 0 || !stbtt_InitFont(&font, font_buf, font_off)) {
        free(font_buf);
        die("\"%s\" is not a valid TTF/OTF font", opt.font);
    }
    float scale = stbtt_ScaleForPixelHeight(&font, (float)opt.size);
    int ascent_u, descent_u, lineGap_u;
    stbtt_GetFontVMetrics(&font, &ascent_u, &descent_u, &lineGap_u);
    int ascent_px      = (int)(ascent_u  * scale + 0.5f);
    int descent_px_abs = (int)(-descent_u * scale + 0.5f);
    int line_h         = (int)((ascent_u - descent_u + lineGap_u) * scale + 0.5f);

    /* Load image. */
    int w = 0, h = 0, ch = 0;
    unsigned char *img = stbi_load(opt.input, &w, &h, &ch, 4);
    if (!img) { free(font_buf); die("cannot decode PNG \"%s\": %s", opt.input, stbi_failure_reason()); }
    if (w <= 0 || h <= 0 || w > MAX_IMG_DIM || h > MAX_IMG_DIM) {
        stbi_image_free(img); free(font_buf);
        die("image dims %dx%d out of range (max %d)", w, h, MAX_IMG_DIM);
    }

    /* Per-line widths. If --x is given, both lines start at that x (left-aligned).
     * Otherwise each line is centered on its own width. */
    long tw1 = measure_text(&font, scale, line1);
    long tw2 = measure_text(&font, scale, line2);
    long block_h = (long)(ascent_px + line_h + descent_px_abs);

    long x1, x2;
    if (opt.have_x) {
        x1 = opt.x;
        x2 = opt.x;
        long tw_max = (tw1 > tw2) ? tw1 : tw2;
        if (opt.x + tw_max > w) {
            stbi_image_free(img); free(font_buf);
            die("text block (%ldx%ld) at (%ld,%ld) would clip image (%dx%d)",
                tw_max, block_h, opt.x, opt.y, w, h);
        }
    } else {
        if (tw1 > w || tw2 > w) {
            stbi_image_free(img); free(font_buf);
            die("text wider than image (line widths %ld,%ld > %d)", tw1, tw2, w);
        }
        x1 = (w - tw1) / 2;
        x2 = (w - tw2) / 2;
    }
    if (opt.y + block_h > h) {
        stbi_image_free(img); free(font_buf);
        die("text block height %ld at y=%ld would clip image height %d",
            block_h, opt.y, h);
    }

    int baseline1 = (int)opt.y + ascent_px;
    int baseline2 = baseline1 + line_h;
    render_line(&font, scale, img, w, h, x1, baseline1, line1, opt.color);
    render_line(&font, scale, img, w, h, x2, baseline2, line2, opt.color);

    /* Write PNG. */
    if (!stbi_write_png(out_path, w, h, 4, img, w * 4)) {
        stbi_image_free(img); free(font_buf);
        die("failed to write \"%s\"", out_path);
    }

    stbi_image_free(img);
    free(font_buf);

    printf(ANSI_GREEN "wrote " ANSI_RESET "%s\n", out_path);
    printf(ANSI_CYAN  "date  " ANSI_RESET "%s\n", line1);
    printf(ANSI_CYAN  "time  " ANSI_RESET "%s\n", line2);

    if (!opt.no_cleanup) {
        const char *new_basename = strrchr(out_path, '/');
        new_basename = new_basename ? new_basename + 1 : out_path;
        cleanup_prior_outputs(opt.output_dir, stem, new_basename);
    }

    return 0;
}
