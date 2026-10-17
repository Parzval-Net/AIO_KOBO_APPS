#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_LCD_WIDTH  160
#define GB_LCD_HEIGHT 144

typedef enum {
  KOBO_DEVICE_UNKNOWN = 0,
  KOBO_DEVICE_CLARA_HD,      /* NXP i.MX6ULL - E-Ink Carta 16-gray 1072x1448 */
  KOBO_DEVICE_LIBRA_COLOUR,  /* MediaTek MT8113T - E-Ink Kaleido 3 CFA 1264x1680 */
  KOBO_DEVICE_GENERIC_BW,
  KOBO_DEVICE_GENERIC_COLOR
} kobo_device_type_t;

typedef enum {
  KOBO_RENDER_QUALITY = 0,   /* Libra Colour: RGB Color (GLRC16/GCC16) | Clara HD: 4-Gray (DU4/GC16) */
  KOBO_RENDER_TURBO_A2 = 1   /* Ambos: 1-bit Ordered Bayer 2x2 con WFM_A2 / WFM_DU ultra rápido */
} kobo_render_mode_t;

/* Waveform modes abstracted across NXP mxcfb (Clara HD) and MTK hwtcon (Libra Colour) */
typedef enum {
  KOBO_WFM_AUTO = 0,
  KOBO_WFM_A2,               /* 1-bit ultra fast (~45ms, zero flash) */
  KOBO_WFM_DU,               /* 1-bit direct update (~55ms, zero flash) */
  KOBO_WFM_DU4,              /* 4-gray direct update (~120ms, Clara HD quality mode) */
  KOBO_WFM_GC16,             /* 16-gray full fidelity (~400ms, anti-ghosting clean) */
  KOBO_WFM_GLRC16,           /* Kaleido 3 fast color (~150ms, Libra Colour gameplay) */
  KOBO_WFM_GCC16             /* Kaleido 3 full color (~450ms, Libra Colour clean flash) */
} kobo_waveform_t;

typedef struct {
  int x, y, w, h;
} kobo_rect_t;

typedef struct {
  int x, y, w, h;
  const char* label;
  uint8_t control_id;
  bool pressed;
} kobo_hud_button_t;

typedef struct {
  kobo_device_type_t device_type;
  bool has_color_panel;
  bool is_mtk;
  bool touch_swap_axes;
  bool touch_mirror_x;
  bool touch_mirror_y;
  uint32_t screen_width;
  uint32_t screen_height;
  uint32_t bpp;
  uint32_t stride_bytes;
  uint8_t current_rota;
  int scale;                 /* Default 6 (960x864, even multiple for Kaleido 3 2x2 CFA), or 7 on Libra */
  int game_x;
  int game_y;
  int game_w;
  int game_h;
  bool portrait;
  kobo_render_mode_t render_mode;
  char device_name[64];
} kobo_display_info_t;

typedef struct {
  uint64_t frames_presented;
  uint64_t frames_skipped_clean;
  uint64_t partial_refreshes;
  uint64_t full_clean_refreshes;
  kobo_rect_t last_dirty_screen_rect;
  kobo_waveform_t last_waveform;
} kobo_display_stats_t;

bool kobo_display_init(kobo_device_type_t simulated_or_override, kobo_display_info_t* out_info);
void kobo_display_shutdown(void);

void kobo_display_set_layout(bool portrait, int preferred_scale, kobo_display_info_t* out_info);
void kobo_display_set_render_mode(kobo_render_mode_t mode);
kobo_render_mode_t kobo_display_get_render_mode(void);

/*
 * Compute 8x8 tile-aligned dirty bounding box between two 160x144 frames.
 * Returns true if at least one pixel changed, writing the game-space rect (0..160, 0..144)
 * to out_game_rect. Returns false if the frame is 100% identical.
 */
bool kobo_display_compute_dirty_rect(const uint16_t* prev_rgb565,
                                     const uint16_t* curr_rgb565,
                                     const uint8_t* prev_shade,
                                     const uint8_t* curr_shade,
                                     bool compare_rgb,
                                     kobo_rect_t* out_game_rect);

/*
 * Present a 160x144 Game Boy / Game Boy Color frame onto the Kobo E-Ink framebuffer.
 * Scales only the dirty region by `scale` (x6 or x7) and triggers an asynchronous
 * EPDC update with the appropriate waveform (GLRC16 / DU4 / A2).
 */
bool kobo_display_present_frame(const uint16_t* rgb565_frame,
                                const uint8_t* shade4_frame,
                                const uint8_t* bw_bayer_frame,
                                bool is_cgb_color,
                                bool force_full_clean);

/* Draw the HUD border, title, status bar, and touch buttons */
void kobo_display_draw_hud(const char* title,
                           const char* status_line,
                           const kobo_hud_button_t* buttons,
                           size_t button_count,
                           bool full_flash);

/* ROM selector list geometry shared by the renderer and the touch hit-testing in main.cpp */
#define KOBO_ROM_LIST_TOP 184
#define KOBO_ROM_ITEM_H   92

/* Number of ROM rows that fit on one selector page for the current screen */
size_t kobo_display_rom_selector_page_size(void);

/*
 * Draw the E-Ink ROM Selector screen, showing the page that contains selected_index.
 * notice (optional) replaces the instruction line, e.g. to report a ROM load error.
 * full_flash=true does a flashing GCC16/GC16 clean; false a non-flashing GC16 update.
 */
void kobo_display_draw_rom_selector(const char* const* rom_names,
                                    size_t rom_count,
                                    size_t selected_index,
                                    const char* device_banner,
                                    const char* footer_hint,
                                    const char* notice,
                                    bool full_flash);

/* Retrieve performance & refresh statistics */
void kobo_display_get_stats(kobo_display_stats_t* out_stats);

/* Export the current simulated/shadow framebuffer to a PPM/PGM image file (for macOS simulator & tests) */
bool kobo_display_save_screenshot(const char* path);

#ifdef __cplusplus
}
#endif
