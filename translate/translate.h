/* === NP2kai internal text translation module ===
 *
 * Inspired by LunaTranslator's "HOOK emulator" + "embedded translation"
 * approach, adapted to the Neko Project II kai PC-98 emulator:
 *
 *   - Text is captured from the emulated machine's TEXT VRAM (character-code
 *     layer) for the "cover original" (覆盖原文) overlay, which gives exact
 *     glyph positions, and from the existing font-render hook (hook_fontrom)
 *     for the "desktop window" (桌面翻译窗) which also catches graphics-layer
 *     text.
 *   - Translation is performed by a pluggable backend (an external command,
 *     e.g. a Python script using an offline/online engine) with an offline
 *     pass-through fallback.
 *
 * This is a frontend-agnostic C module; the wx port renders the two outputs.
 */

#ifndef NP2_TRANSLATE_H
#define NP2_TRANSLATE_H

#include <compiler.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Output mode (bit flags so OVERLAY + WINDOW can be combined if wanted). */
typedef enum {
	TR_MODE_OFF     = 0,
	TR_MODE_OVERLAY = 1,   /* 覆盖原文 */
	TR_MODE_WINDOW  = 2    /* 桌面翻译窗 */
} tr_mode_t;

#define TR_OVERLAY_MAX   320
#define TR_WINDOW_LINES  320
#define TR_SRC_MAX      1536
#define TR_TRANS_MAX    6144
#define TR_CACHE_SIZE   8192

/* A captured text line with a screen rectangle (native, pre-rotation pixels). */
typedef struct {
	int		x, y, w, h;          /* native screen rectangle */
	int		ready;               /* 1 when translation is available */
	char	orig[TR_SRC_MAX];    /* original text (UTF-8) */
	char	tran[TR_TRANS_MAX];  /* translation (UTF-8); empty if pending */
} tr_overlay_t;

/* lifecycle */
void translate_init(void);
void translate_term(void);

/* configuration */
void translate_set_mode(tr_mode_t mode);
tr_mode_t translate_get_mode(void);
void translate_set_engine(const char *cmd);     /* command; NULL/"" = offline */
const char *translate_get_engine(void);
void translate_clear(void);
void translate_set_screensize(int w, int h);

/* capture entry points */
void translate_scan_textvram(void);             /* called each frame (overlay) */
void translate_feed_line(const char *utf8);     /* hook_fontrom output (window) */
void translate_pump(void);                      /* backend worker (UI timer) */

/* UI accessors */
int  translate_get_overlay(tr_overlay_t *out, int max);
int  translate_get_window_count(void);
void translate_get_window_line(int idx, char *orig, char *tran, int max);

/* callback set by the UI; invoked when a new translation arrives */
void translate_set_redraw_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* NP2_TRANSLATE_H */
