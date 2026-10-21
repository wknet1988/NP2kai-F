/* === NP2kai internal text translation module (implementation) ===
 *
 * Captures text from the emulated PC-98 TEXT VRAM for the "cover original"
 * overlay, and from the always-compiled font-render hook (hook_fontrom) for
 * the "desktop window".  Translation is performed by a pluggable external
 * command; with no engine configured the original text is echoed (offline
 * pass-through) so the capture/display pipeline is always demonstrable.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <process.h>            /* getpid() */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>            /* CRITICAL_SECTION, system() */

#include <compiler.h>
#include <cpucore.h>
#include <pccore.h>
#include <io/iocore.h>
#include <io/gdc.h>
#include <vram/vram.h>
#include <vram/scrndraw.h>
#include <vram/dispsync.h>
#include <vram/maketext.h>
#include <font/font.h>
#include <codecnv/codecnv.h>

#include "translate.h"

#ifndef LOW12
#define LOW12(x) ((x) & 0x0fff)
#endif

/* ---- shared state (guarded by g_cs) ---- */

static CRITICAL_SECTION g_cs;

static tr_mode_t    g_mode      = TR_MODE_OFF;
static char         g_engine[1024];
static int          g_sw        = 640;
static int          g_sh        = 400;

static tr_overlay_t g_overlay[TR_OVERLAY_MAX];
static int          g_overlay_cnt = 0;

/* desktop-window ring buffer */
typedef struct {
	char	orig[TR_SRC_MAX];
	char	tran[TR_TRANS_MAX];
} tr_winline_t;

static tr_winline_t g_win[TR_WINDOW_LINES];
static int          g_win_head = 0;   /* next write position */
static int          g_win_count = 0;  /* number of valid entries */
static int          g_win_prev_match = 0;

/* translation cache (open addressing) */
typedef struct {
	char	*key;     /* original text (malloc) */
	char	*val;     /* translation (malloc) or NULL while pending */
	int		used;
} tr_cache_t;

static tr_cache_t   g_cache[TR_CACHE_SIZE];

static void       (*g_redraw_cb)(void) = NULL;

/* ---- helpers ---- */

static char *tr_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char  *p = (char *)malloc(n);
	if (p) memcpy(p, s, n);
	return p;
}

static unsigned long tr_hash(const char *s)
{
	unsigned long h = 5381;
	int c;
	while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c; /* djb2 */
	return h;
}

/* returns cache index for key (inserting empty slot if missing) */
static int tr_cache_getidx(const char *key)
{
	unsigned long h = tr_hash(key) % TR_CACHE_SIZE;
	int i;
	for (i = 0; i < TR_CACHE_SIZE; i++) {
		int idx = (int)((h + (unsigned long)i) % TR_CACHE_SIZE);
		if (!g_cache[idx].used)
			return idx;                 /* free slot */
		if (g_cache[idx].key && strcmp(g_cache[idx].key, key) == 0)
			return idx;                 /* hit */
	}
	return -1;                              /* table full */
}

/* ---- public API ---- */

void translate_init(void)
{
	InitializeCriticalSection(&g_cs);
	g_mode = TR_MODE_OFF;
	g_engine[0] = '\0';
	g_overlay_cnt = 0;
	g_win_head = 0;
	g_win_count = 0;
	memset(g_cache, 0, sizeof(g_cache));
}

void translate_term(void)
{
	int i;
	for (i = 0; i < TR_CACHE_SIZE; i++) {
		if (g_cache[i].key) free(g_cache[i].key);
		if (g_cache[i].val) free(g_cache[i].val);
	}
	DeleteCriticalSection(&g_cs);
}

void translate_set_mode(tr_mode_t mode)
{
	int was_window = (g_mode & TR_MODE_WINDOW);
	int now_window = (mode & TR_MODE_WINDOW);

	g_mode = mode;

	/* The desktop window is fed by hook_fontrom; enable/disable it here so it
	 * is the single owner of the font hook. */
	if (now_window && !was_window) {
		hook_fontrom_setoutput(translate_feed_line);
		hf_enable = 1;
		g_win_count = 0;
		g_win_head = 0;
	}
	else if (!now_window && was_window && !(g_mode & TR_MODE_WINDOW)) {
		hf_enable = 0;
	}
}

tr_mode_t translate_get_mode(void)
{
	return g_mode;
}

void translate_set_engine(const char *cmd)
{
	if (cmd) {
		strncpy(g_engine, cmd, sizeof(g_engine) - 1);
		g_engine[sizeof(g_engine) - 1] = '\0';
	}
	else {
		g_engine[0] = '\0';
	}
}

const char *translate_get_engine(void)
{
	return g_engine;
}

void translate_clear(void)
{
	int i;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < TR_CACHE_SIZE; i++) {
		if (g_cache[i].key) { free(g_cache[i].key); g_cache[i].key = NULL; }
		if (g_cache[i].val) { free(g_cache[i].val); g_cache[i].val = NULL; }
		g_cache[i].used = 0;
	}
	g_overlay_cnt = 0;
	g_win_count = 0;
	g_win_head = 0;
	LeaveCriticalSection(&g_cs);
}

void translate_set_screensize(int w, int h)
{
	g_sw = w;
	g_sh = h;
}

void translate_set_redraw_cb(void (*cb)(void))
{
	g_redraw_cb = cb;
}

/* ---- JIS (ESC-encoded) -> UTF-8 conversion (reuses existing codecnv) ---- */

static void jisbuf_ensure_type(char **pp, char *end, UINT *curtype, UINT newtype)
{
	char *p = *pp;
	if (*curtype == newtype)
		return;
	if (p + 3 <= end) {
		*p++ = 0x1B;                       /* ESC */
		if (newtype == 2) {
			*p++ = 0x24;                   /* $ */
			*p++ = 0x42;                   /* B */
		}
		else {
			*p++ = 0x28;                   /* ( */
			*p++ = (newtype == 1) ? 0x49 : 0x42;  /* I or B */
		}
	}
	*curtype = newtype;
	*pp = p;
}

static void tr_conv_line(const char *jisbuf, int jislen, char *out, int outsize)
{
	static UINT16 u16[TR_SRC_MAX];
	UINT type = 0;
	out[0] = '\0';
	codecnv_jistoucs2(&type, u16, TR_SRC_MAX, jisbuf, -1, 0);
	codecnv_ucs2toutf8(out, outsize, u16, -1);
}

/* ---- capture from TEXT VRAM (overlay source) ---- */

void translate_scan_textvram(void)
{
	tr_mode_t mode;
	int width80, ncols, cellw, step, cellh;
	UINT pitch, esi0;
	int area_top, area_bottom, maxrows;
	int r, c;

	mode = translate_get_mode();
	if (!(mode & TR_MODE_OVERLAY))
		return;

	if (!mem)                 /* emulator memory not yet allocated */
		return;

	width80 = !(gdc.mode1 & 0x4);
	ncols  = width80 ? 80 : 40;
	cellw  = width80 ? 8  : 16;
	step   = width80 ? 1  : 2;
	pitch  = (UINT)(gdc.m.para[GDC_PITCH] & 0xfe);
	esi0   = LOW12(gdc.m.para[GDC_SCROLL] |
	              ((UINT)gdc.m.para[GDC_SCROLL + 1] << 8));

	cellh = (int)crtc.reg.cl;
	if (cellh < 8)  cellh = 16;
	if (cellh > 32) cellh = 32;

	area_top    = (int)dsync.text_vbp;
	area_bottom = (int)dsync.textymax;
	maxrows = (area_bottom - area_top) / cellh;
	if (maxrows > 100) maxrows = 100;

	EnterCriticalSection(&g_cs);
	g_overlay_cnt = 0;

	for (r = 0; r < maxrows; r++) {
		UINT edi = LOW12((UINT)esi0 + (UINT)r * pitch);
		int  first = -1, last = -1;
		char jisbuf[TR_SRC_MAX * 2];
		char *jp = jisbuf;
		char *jend = jisbuf + sizeof(jisbuf) - 8;
		UINT  curtype = 0;
		int   kanjiTrail = 0;

		for (c = 0; c < ncols; c++) {
			UINT8 attr, code0, code1;
			if (kanjiTrail) {
				kanjiTrail = 0;
				edi = LOW12(edi + step);
				continue;
			}
			attr  = mem[0xa2000 + edi * 2];
			code0 = mem[0xa0000 + edi * 2];
			code1 = mem[0xa0001 + edi * 2];

			if (attr & gdc.bitac) {
				/* kanji lead: 2-byte JIS code held by this cell */
				UINT16 kw = (UINT16)(code0 | (code1 << 8));
				UINT ua  = kw & 0x7F7F;
				int jhi  = (int)((ua & 0x7F) + 0x20);
				int jlo  = (int)((ua >> 8) & 0x7F);
				jisbuf_ensure_type(&jp, jend, &curtype, 2);
				if (jp + 2 <= jend) {
					*jp++ = (char)(jhi & 0x7F);
					*jp++ = (char)(jlo & 0x7F);
				}
				if (first < 0) first = c;
				last = c;
				kanjiTrail = 1;            /* next cell is the trail byte */
			}
			else {
				int b = code0;
				if (b <= 0x1F || b == 0x7F) {
					jisbuf_ensure_type(&jp, jend, &curtype, 0);
					if (jp + 1 <= jend) *jp++ = 0x20;   /* treat as space */
				}
				else if (b >= 0x20 && b <= 0x7E) {
					jisbuf_ensure_type(&jp, jend, &curtype, 0);
					if (jp + 1 <= jend) *jp++ = (char)b;
					if (first < 0) first = c;
					last = c;
				}
				else if (b >= 0xA1 && b <= 0xDF) {
					/* half-width katakana */
					jisbuf_ensure_type(&jp, jend, &curtype, 1);
					if (jp + 1 <= jend) *jp++ = (char)b;
					if (first < 0) first = c;
					last = c;
				}
				else {
					jisbuf_ensure_type(&jp, jend, &curtype, 0);
					if (jp + 1 <= jend) *jp++ = 0x20;
				}
			}
			edi = LOW12(edi + step);
		}
		*jp = '\0';

		if (first < 0)
			continue;   /* blank line */

		{
			char orig[TR_SRC_MAX];
			tr_overlay_t *o;
			int idx;

			tr_conv_line(jisbuf, (int)(jp - jisbuf), orig, sizeof(orig));
			if (orig[0] == '\0')
				continue;

			if (g_overlay_cnt >= TR_OVERLAY_MAX)
				break;

			o = &g_overlay[g_overlay_cnt];
			o->x = (int)dsync.scrnxpos + first * cellw;
			o->y = area_top + r * cellh;
			o->w = (last - first + 1) * cellw;
			o->h = cellh;
			strncpy(o->orig, orig, TR_SRC_MAX - 1);
			o->orig[TR_SRC_MAX - 1] = '\0';
			o->ready = 0;
			o->tran[0] = '\0';

			/* look up / insert into cache */
			idx = tr_cache_getidx(o->orig);
			if (idx >= 0) {
				if (!g_cache[idx].used) {
					g_cache[idx].key = tr_strdup(o->orig);
					g_cache[idx].val = NULL;     /* pending */
					g_cache[idx].used = 1;
				}
				if (g_cache[idx].val) {
					strncpy(o->tran, g_cache[idx].val, TR_TRANS_MAX - 1);
					o->tran[TR_TRANS_MAX - 1] = '\0';
					o->ready = 1;
				}
			}
			g_overlay_cnt++;
		}
	}
	LeaveCriticalSection(&g_cs);
}

/* ---- capture from font hook (desktop window source) ---- */

void translate_feed_line(const char *utf8)
{
	size_t len;
	if (!utf8) return;
	len = strlen(utf8);
	if (len == 0) return;

	EnterCriticalSection(&g_cs);
	/* collapse consecutive identical runs */
	if (g_win_count > 0) {
		int last_idx = (g_win_head - 1 + TR_WINDOW_LINES) % TR_WINDOW_LINES;
		if (strcmp(g_win[last_idx].orig, utf8) == 0) {
			LeaveCriticalSection(&g_cs);
			return;
		}
	}
	strncpy(g_win[g_win_head].orig, utf8, TR_SRC_MAX - 1);
	g_win[g_win_head].orig[TR_SRC_MAX - 1] = '\0';
	/* window translation: reuse the same cache as the overlay */
	{
		int idx = tr_cache_getidx(g_win[g_win_head].orig);
		if (idx >= 0) {
			if (!g_cache[idx].used) {
				g_cache[idx].key = tr_strdup(g_win[g_win_head].orig);
				g_cache[idx].val = NULL;
				g_cache[idx].used = 1;
			}
			if (g_cache[idx].val)
				strncpy(g_win[g_win_head].tran, g_cache[idx].val, TR_TRANS_MAX - 1);
			else
				g_win[g_win_head].tran[0] = '\0';
		}
		else {
			g_win[g_win_head].tran[0] = '\0';
		}
	}
	g_win_head = (g_win_head + 1) % TR_WINDOW_LINES;
	if (g_win_count < TR_WINDOW_LINES) g_win_count++;
	LeaveCriticalSection(&g_cs);
}

/* ---- translation backend pump (called from UI timer) ---- */

static void tr_write_file(const char *path, char **lines, int n)
{
	FILE *f = fopen(path, "wb");
	int i;
	if (!f) return;
	for (i = 0; i < n; i++) {
		fprintf(f, "%s\n", lines[i] ? lines[i] : "");
	}
	fclose(f);
}

static int tr_read_lines(const char *path, char **out, int max)
{
	FILE *f = fopen(path, "rb");
	int n = 0;
	char buf[TR_TRANS_MAX];
	if (!f) return 0;
	while (n < max && fgets(buf, sizeof(buf), f)) {
		size_t l = strlen(buf);
		while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
		out[n] = tr_strdup(buf);
		n++;
	}
	fclose(f);
	return n;
}

void translate_pump(void)
{
	int idxs[1024];
	char *keys[1024];
	int n = 0;
	int i;

	/* gather pending (cache entries with val == NULL) */
	EnterCriticalSection(&g_cs);
	for (i = 0; i < TR_CACHE_SIZE && n < 1024; i++) {
		if (g_cache[i].used && g_cache[i].val == NULL && g_cache[i].key) {
			idxs[n] = i;
			keys[n] = g_cache[i].key;
			n++;
		}
	}
	LeaveCriticalSection(&g_cs);

	if (n == 0)
		return;

	if (g_engine[0] == '\0') {
		/* offline pass-through: echo original as translation */
		EnterCriticalSection(&g_cs);
		for (i = 0; i < n; i++) {
			int idx = idxs[i];
			if (g_cache[idx].val == NULL && g_cache[idx].key)
				g_cache[idx].val = tr_strdup(g_cache[idx].key);
		}
		LeaveCriticalSection(&g_cs);
	}
	else {
		char inpath[MAX_PATH], outpath[MAX_PATH];
		char tmpdir[MAX_PATH];
		char cmd[2048];
		char *outlines[1024];
		int got, k;
		const char *t = getenv("TMP");
		if (!t) t = getenv("TEMP");
		if (!t) t = ".";
		_snprintf(tmpdir, sizeof(tmpdir), "%s", t);
		_snprintf(inpath,  sizeof(inpath),  "%s\\np2tr_%u_in.txt",  tmpdir, (unsigned)getpid());
		_snprintf(outpath, sizeof(outpath), "%s\\np2tr_%u_out.txt", tmpdir, (unsigned)getpid());

		tr_write_file(inpath, keys, n);

		/* engine is invoked as:  <engine> <infile> <outfile> */
		_snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\"", g_engine, inpath, outpath);
		system(cmd);

		for (k = 0; k < n; k++) outlines[k] = NULL;
		got = tr_read_lines(outpath, outlines, n);

		EnterCriticalSection(&g_cs);
		for (i = 0; i < n; i++) {
			int idx = idxs[i];
			char *v;
			if (g_cache[idx].val != NULL) continue;     /* already filled */
			if (i < got && outlines[i] && outlines[i][0])
				v = tr_strdup(outlines[i]);
			else
				v = tr_strdup(g_cache[idx].key);          /* fallback echo */
			g_cache[idx].val = v;
		}
		LeaveCriticalSection(&g_cs);

		for (k = 0; k < got; k++) if (outlines[k]) free(outlines[k]);
		DeleteFileA(inpath);
		DeleteFileA(outpath);
	}

	/* refresh window entries that were just translated */
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_win_count; i++) {
		int wi = (g_win_head - g_win_count + i + TR_WINDOW_LINES) % TR_WINDOW_LINES;
		int cidx = tr_cache_getidx(g_win[wi].orig);
		if (cidx >= 0 && g_cache[cidx].val) {
			strncpy(g_win[wi].tran, g_cache[cidx].val, TR_TRANS_MAX - 1);
			g_win[wi].tran[TR_TRANS_MAX - 1] = '\0';
		}
	}
	LeaveCriticalSection(&g_cs);

	if (g_redraw_cb)
		g_redraw_cb();
}

/* ---- UI accessors ---- */

int translate_get_overlay(tr_overlay_t *out, int max)
{
	int i, n;
	EnterCriticalSection(&g_cs);
	n = (g_overlay_cnt < max) ? g_overlay_cnt : max;
	for (i = 0; i < n; i++)
		out[i] = g_overlay[i];
	LeaveCriticalSection(&g_cs);
	return n;
}

int translate_get_window_count(void)
{
	int n;
	EnterCriticalSection(&g_cs);
	n = g_win_count;
	LeaveCriticalSection(&g_cs);
	return n;
}

void translate_get_window_line(int idx, char *orig, char *tran, int max)
{
	int wi;
	EnterCriticalSection(&g_cs);
	if (idx < 0 || idx >= g_win_count) {
		orig[0] = '\0';
		tran[0] = '\0';
	}
	else {
		wi = (g_win_head - g_win_count + idx + TR_WINDOW_LINES) % TR_WINDOW_LINES;
		strncpy(orig, g_win[wi].orig, max - 1);
		orig[max - 1] = '\0';
		strncpy(tran, g_win[wi].tran, max - 1);
		tran[max - 1] = '\0';
	}
	LeaveCriticalSection(&g_cs);
}
