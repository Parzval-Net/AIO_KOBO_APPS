#include "platform/kobo_display.h"
#include "core/walnut_cgb_color.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(HAVE_FBINK)
#include <fbink.h>
#endif

/* Fallback ioctl definitions for MTK hwtcon (Libra Colour) and NXP mxcfb v1/v2 (Clara HD) */
struct mxcfb_rect_ {
  uint32_t top;
  uint32_t left;
  uint32_t width;
  uint32_t height;
};

struct hwtcon_update_data_local {
  struct mxcfb_rect_ update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  unsigned int flags;
  int dither_mode;
};

struct mxcfb_alt_buffer_data_v2_local {
  uint32_t phys_addr;
  uint32_t width;
  uint32_t height;
  struct mxcfb_rect_ alt_update_region;
};

struct mxcfb_update_data_v2_local {
  struct mxcfb_rect_ update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int temp;
  unsigned int flags;
  int dither_mode;
  int quant_bit;
  struct mxcfb_alt_buffer_data_v2_local alt_buffer_data;
};

#define LOCAL_HWTCON_SEND_UPDATE   _IOW('F', 0x2E, struct hwtcon_update_data_local)
#define LOCAL_MXCFB_SEND_UPDATE_V2 _IOW('F', 0x2E, struct mxcfb_update_data_v2_local)
#define UPDATE_MODE_PARTIAL_LOCAL  0x0u
#define UPDATE_MODE_FULL_LOCAL     0x1u
#define TEMP_USE_AMBIENT_LOCAL     0x1000
#endif

namespace {

/* Compact 8x8 bitmap font for ASCII 32..95 (uppercase, digits, punctuation) for E-Ink HUD & ROM selector */
const uint8_t kFont8x8[64][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},  // '!'
    {0x6C, 0x6C, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00},  // '"'
    {0x6C, 0xFE, 0x6C, 0x6C, 0xFE, 0x6C, 0x00, 0x00},  // '#'
    {0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00},  // '$'
    {0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00},  // '%'
    {0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00},  // '&'
    {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},  // '\''
    {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00},  // '('
    {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00},  // ')'
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},  // '*'
    {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00},  // '+'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30},  // ','
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},  // '-'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00},  // '.'
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00},  // '/'
    {0x7C, 0xC6, 0xCE, 0xD6, 0xE6, 0xC6, 0x7C, 0x00},  // '0'
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},  // '1'
    {0x7C, 0xC6, 0x06, 0x1C, 0x30, 0x66, 0xFE, 0x00},  // '2'
    {0x7C, 0xC6, 0x06, 0x3C, 0x06, 0xC6, 0x7C, 0x00},  // '3'
    {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x1E, 0x00},  // '4'
    {0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00},  // '5'
    {0x38, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00},  // '6'
    {0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00},  // '7'
    {0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00},  // '8'
    {0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00},  // '9'
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},  // ':'
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30},  // ';'
    {0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00},  // '<'
    {0x00, 0x00, 0x7E, 0x00, 0x00, 0x7E, 0x00, 0x00},  // '='
    {0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00},  // '>'
    {0x7C, 0xC6, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x00},  // '?'
    {0x7C, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x78, 0x00},  // '@'
    {0x38, 0x6C, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00},  // 'A'
    {0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00},  // 'B'
    {0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00},  // 'C'
    {0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00},  // 'D'
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00},  // 'E'
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00},  // 'F'
    {0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00},  // 'G'
    {0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00},  // 'H'
    {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00},  // 'I'
    {0x1E, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, 0x00},  // 'J'
    {0xE6, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0xE6, 0x00},  // 'K'
    {0xF0, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00},  // 'L'
    {0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00},  // 'M'
    {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00},  // 'N'
    {0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00},  // 'O'
    {0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00},  // 'P'
    {0x7C, 0xC6, 0xC6, 0xC6, 0xD6, 0xDE, 0x7C, 0x0E},  // 'Q'
    {0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0xE6, 0x00},  // 'R'
    {0x7C, 0xC6, 0x60, 0x38, 0x0C, 0xC6, 0x7C, 0x00},  // 'S'
    {0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00},  // 'T'
    {0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00},  // 'U'
    {0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00},  // 'V'
    {0xC6, 0xC6, 0xD6, 0xFE, 0xFE, 0xEE, 0xC6, 0x00},  // 'W'
    {0xC6, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0xC6, 0x00},  // 'X'
    {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x3C, 0x00},  // 'Y'
    {0xFE, 0xC6, 0x8C, 0x18, 0x32, 0x66, 0xFE, 0x00},  // 'Z'
    {0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00},  // '['
    {0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00},  // '\\'
    {0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00},  // ']'
    {0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00},  // '^'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},  // '_'
};

struct DisplayContext {
  bool initialized = false;
  kobo_display_info_t info = {};
  kobo_display_stats_t stats = {};

  // Shadow 32-bit BGRA8888 surface (used both for macOS simulator and for blitting to Kobo /dev/fb0)
  std::vector<uint32_t> surface_bgra;
  std::vector<uint8_t> rgba_staging;

  // Previous 160x144 frame caches for 8x8 tile dirty-rectangle detection
  bool has_prev_frame = false;
  uint16_t prev_rgb565[GB_LCD_WIDTH * GB_LCD_HEIGHT] = {};
  uint8_t prev_shade[GB_LCD_WIDTH * GB_LCD_HEIGHT] = {};

#if defined(__linux__)
  int fbfd = -1;
  uint8_t* fb_mmap = nullptr;
  size_t fb_mmap_size = 0;
  uint32_t next_marker = 1;
#if defined(HAVE_FBINK)
  FBInkConfig fbink_cfg = {};
  FBInkState fbink_state = {};
  FBInkDump nickel_dump = {};
  bool has_nickel_dump = false;
#endif
#endif
};

DisplayContext g_disp;

kobo_device_type_t detectKoboHardware() {
  const char* env_dev = std::getenv("INKBOY_KOBO_DEVICE");
  if (env_dev) {
    if (std::strstr(env_dev, "libra") || std::strstr(env_dev, "390") || std::strstr(env_dev, "monza")) {
      return KOBO_DEVICE_LIBRA_COLOUR;
    }
    if (std::strstr(env_dev, "clara") || std::strstr(env_dev, "376") || std::strstr(env_dev, "nova")) {
      return KOBO_DEVICE_CLARA_HD;
    }
  }

  FILE* fp = std::fopen("/mnt/onboard/.kobo/version", "r");
  if (fp) {
    char line[256] = {};
    if (std::fgets(line, sizeof(line), fp)) {
      std::fclose(fp);
      if (std::strstr(line, "000000000390")) return KOBO_DEVICE_LIBRA_COLOUR;
      if (std::strstr(line, "000000000393")) return KOBO_DEVICE_LIBRA_COLOUR;
      if (std::strstr(line, "000000000376")) return KOBO_DEVICE_CLARA_HD;
    } else {
      std::fclose(fp);
    }
  }
  return KOBO_DEVICE_LIBRA_COLOUR;
}

void fillRectSurface(int x, int y, int w, int h, uint32_t bgra) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(static_cast<int>(g_disp.info.screen_width), x + w);
  const int y1 = std::min(static_cast<int>(g_disp.info.screen_height), y + h);
  for (int py = y0; py < y1; ++py) {
    uint32_t* row = &g_disp.surface_bgra[static_cast<size_t>(py) * g_disp.info.screen_width];
    for (int px = x0; px < x1; ++px) {
      row[px] = bgra;
    }
  }
}

void drawRectBorder(int x, int y, int w, int h, int thickness, uint32_t bgra) {
  fillRectSurface(x, y, w, thickness, bgra);
  fillRectSurface(x, y + h - thickness, w, thickness, bgra);
  fillRectSurface(x, y, thickness, h, bgra);
  fillRectSurface(x + w - thickness, y, thickness, h, bgra);
}

void drawCharScaled(int x, int y, char c, int font_scale, uint32_t bgra) {
  unsigned char uc = static_cast<unsigned char>(c);
  if (uc >= 'a' && uc <= 'z') uc = static_cast<unsigned char>(uc - 'a' + 'A');
  if (uc < 32 || uc > 95) uc = '?';
  const uint8_t* glyph = kFont8x8[uc - 32];
  for (int gy = 0; gy < 8; ++gy) {
    const uint8_t row_bits = glyph[gy];
    for (int gx = 0; gx < 8; ++gx) {
      if ((row_bits & (0x80U >> gx)) != 0) {
        fillRectSurface(x + gx * font_scale, y + gy * font_scale, font_scale, font_scale, bgra);
      }
    }
  }
}

void drawTextScaled(int x, int y, const char* text, int font_scale, uint32_t bgra) {
  if (!text) return;
  int cx = x;
  for (const char* p = text; *p; ++p) {
    drawCharScaled(cx, y, *p, font_scale, bgra);
    cx += 8 * font_scale + font_scale;
  }
}

int measureTextWidth(const char* text, int font_scale) {
  if (!text || !*text) return 0;
  const int len = static_cast<int>(std::strlen(text));
  return len * (8 * font_scale) + (len - 1) * font_scale;
}

void triggerEpdcRefresh(const kobo_rect_t& rect, kobo_waveform_t wfm, bool full_flash) {
  g_disp.stats.last_dirty_screen_rect = rect;
  g_disp.stats.last_waveform = wfm;
  if (full_flash) {
    ++g_disp.stats.full_clean_refreshes;
  } else {
    ++g_disp.stats.partial_refreshes;
  }

#if defined(__linux__)
  const int x0 = std::max(0, rect.x);
  const int y0 = std::max(0, rect.y);
  const int x1 = std::min(static_cast<int>(g_disp.info.screen_width), rect.x + rect.w);
  const int y1 = std::min(static_cast<int>(g_disp.info.screen_height), rect.y + rect.h);
  const int rw = x1 - x0;
  const int rh = y1 - y0;
  if (rw <= 1 || rh <= 1) return;

#if defined(HAVE_FBINK)
  if (g_disp.fbfd >= 0) {
    // Convert dirty rectangle from internal BGRA8888 surface to packed RGBA8888 for fbink_print_raw_data
    const size_t total_bytes = static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4U;
    if (g_disp.rgba_staging.size() < total_bytes) {
      g_disp.rgba_staging.resize(total_bytes);
    }
    uint8_t* dst = g_disp.rgba_staging.data();
    for (int y = y0; y < y1; ++y) {
      const uint32_t* src_row = &g_disp.surface_bgra[static_cast<size_t>(y) * g_disp.info.screen_width];
      for (int x = x0; x < x1; ++x) {
        const uint32_t bgra = src_row[x];
        dst[0] = static_cast<uint8_t>((bgra >> 16) & 0xFFU);  // R
        dst[1] = static_cast<uint8_t>((bgra >> 8) & 0xFFU);   // G
        dst[2] = static_cast<uint8_t>(bgra & 0xFFU);          // B
        dst[3] = 0xFFU;                                       // A
        dst += 4;
      }
    }

    FBInkConfig cfg = g_disp.fbink_cfg;
    cfg.ignore_alpha = true;
    cfg.no_refresh = false;
    cfg.is_flashing = full_flash;

    switch (wfm) {
      case KOBO_WFM_A2:
      case KOBO_WFM_DU:
        // Prefer WFM_DU for 1-bit fast updates: same ~45ms speed on Kobo without Mk7 FORCE_MONOCHROME flag quirk
        cfg.wfm_mode = WFM_DU;
        cfg.is_flashing = false;
        break;
      case KOBO_WFM_DU4:
        cfg.wfm_mode = WFM_DU4;
        cfg.is_flashing = full_flash;
        break;
      case KOBO_WFM_GLRC16:
        cfg.wfm_mode = WFM_GLRC16;
        cfg.cfa_mode = CFA_G2;
        // On MediaTek Kaleido 3 (Libra Colour), GLRC16 is a REAGL waveform that requires UPDATE_MODE_FULL (doesn't flash)
        cfg.is_flashing = g_disp.info.is_mtk ? true : full_flash;
        break;
      case KOBO_WFM_GCC16:
        cfg.wfm_mode = WFM_GCC16;
        cfg.cfa_mode = CFA_G2;
        cfg.is_flashing = true;
        break;
      default:
        cfg.wfm_mode = WFM_GC16;
        cfg.is_flashing = full_flash;
        break;
    }

    fbink_print_raw_data(g_disp.fbfd, g_disp.rgba_staging.data(), rw, rh, total_bytes,
                         static_cast<short int>(x0), static_cast<short int>(y0), &cfg);
    return;
  }
#endif

  if (g_disp.fbfd >= 0 && g_disp.fb_mmap) {
    for (int y = y0; y < y1; ++y) {
      const uint32_t* src_row = &g_disp.surface_bgra[static_cast<size_t>(y) * g_disp.info.screen_width];
      uint8_t* dst_row = g_disp.fb_mmap + static_cast<size_t>(y) * g_disp.info.stride_bytes;
      if (g_disp.info.bpp == 32) {
        std::memcpy(dst_row + static_cast<size_t>(x0) * 4, src_row + x0,
                    static_cast<size_t>(rw) * sizeof(uint32_t));
      } else if (g_disp.info.bpp == 16) {
        uint16_t* dst16 = reinterpret_cast<uint16_t*>(dst_row);
        for (int x = x0; x < x1; ++x) {
          const uint32_t p = src_row[x];
          const uint8_t r = static_cast<uint8_t>((p >> 16) & 0xFF);
          const uint8_t g = static_cast<uint8_t>((p >> 8) & 0xFF);
          const uint8_t b = static_cast<uint8_t>(p & 0xFF);
          dst16[x] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
      } else if (g_disp.info.bpp == 8) {
        for (int x = x0; x < x1; ++x) {
          const uint32_t p = src_row[x];
          const uint8_t r = static_cast<uint8_t>((p >> 16) & 0xFF);
          const uint8_t g = static_cast<uint8_t>((p >> 8) & 0xFF);
          const uint8_t b = static_cast<uint8_t>(p & 0xFF);
          dst_row[x] = static_cast<uint8_t>((299U * r + 587U * g + 114U * b) / 1000U);
        }
      }
    }

    const uint32_t marker = g_disp.next_marker++;
    if (g_disp.info.is_mtk) {
      struct hwtcon_update_data_local mtk_upd {};
      mtk_upd.update_region = {static_cast<uint32_t>(y0), static_cast<uint32_t>(x0),
                               static_cast<uint32_t>(rw), static_cast<uint32_t>(rh)};
      mtk_upd.waveform_mode = (wfm == KOBO_WFM_A2 || wfm == KOBO_WFM_DU) ? 1U : 11U;
      mtk_upd.update_mode = (full_flash || mtk_upd.waveform_mode == 11U) ? UPDATE_MODE_FULL_LOCAL : UPDATE_MODE_PARTIAL_LOCAL;
      mtk_upd.update_marker = marker;
      ::ioctl(g_disp.fbfd, LOCAL_HWTCON_SEND_UPDATE, &mtk_upd);
    } else {
      struct mxcfb_update_data_v2_local nxp_upd {};
      nxp_upd.update_region = {static_cast<uint32_t>(y0), static_cast<uint32_t>(x0),
                               static_cast<uint32_t>(rw), static_cast<uint32_t>(rh)};
      nxp_upd.waveform_mode = (wfm == KOBO_WFM_A2 || wfm == KOBO_WFM_DU) ? 1U : 2U;
      nxp_upd.update_mode = full_flash ? UPDATE_MODE_FULL_LOCAL : UPDATE_MODE_PARTIAL_LOCAL;
      nxp_upd.temp = TEMP_USE_AMBIENT_LOCAL;
      nxp_upd.update_marker = marker;
      ::ioctl(g_disp.fbfd, LOCAL_MXCFB_SEND_UPDATE_V2, &nxp_upd);
    }
  }
#endif
}

}  // namespace

extern "C" {

bool kobo_display_init(kobo_device_type_t simulated_or_override, kobo_display_info_t* out_info) {
  g_disp = DisplayContext{};

  kobo_device_type_t dev = simulated_or_override;
  if (dev == KOBO_DEVICE_UNKNOWN) {
    dev = detectKoboHardware();
  }

  g_disp.info.device_type = dev;
  if (dev == KOBO_DEVICE_CLARA_HD || dev == KOBO_DEVICE_GENERIC_BW) {
    g_disp.info.has_color_panel = false;
    g_disp.info.is_mtk = false;
    g_disp.info.touch_swap_axes = true;
    g_disp.info.touch_mirror_x = true;
    g_disp.info.touch_mirror_y = false;
    g_disp.info.screen_width = 1072;
    g_disp.info.screen_height = 1448;
    g_disp.info.bpp = 8;
    g_disp.info.stride_bytes = 1072;
    std::snprintf(g_disp.info.device_name, sizeof(g_disp.info.device_name),
                  "Kobo Clara HD (i.MX6ULL Carta 16-Gray)");
  } else {
    g_disp.info.has_color_panel = true;
    g_disp.info.is_mtk = true;
    g_disp.info.touch_swap_axes = true;
    g_disp.info.touch_mirror_x = false;
    g_disp.info.touch_mirror_y = true;
    g_disp.info.screen_width = 1264;
    g_disp.info.screen_height = 1680;
    g_disp.info.bpp = 32;
    g_disp.info.stride_bytes = 1264 * 4;
    std::snprintf(g_disp.info.device_name, sizeof(g_disp.info.device_name),
                  "Kobo Libra Colour (MT8113T Kaleido 3 CFA)");
  }

#if defined(__linux__)
#if defined(HAVE_FBINK)
  g_disp.fbfd = fbink_open();
  if (g_disp.fbfd >= 0) {
    g_disp.fbink_cfg.is_quiet = false;
    if (fbink_init(g_disp.fbfd, &g_disp.fbink_cfg) >= 0) {
      fbink_get_state(&g_disp.fbink_cfg, &g_disp.fbink_state);
      if (g_disp.fbink_state.screen_width > 0 && g_disp.fbink_state.screen_height > 0) {
        g_disp.info.screen_width = g_disp.fbink_state.screen_width;
        g_disp.info.screen_height = g_disp.fbink_state.screen_height;
      }
      g_disp.info.bpp = g_disp.fbink_state.bpp;
      g_disp.info.stride_bytes = g_disp.fbink_state.scanline_stride;
      g_disp.info.current_rota = g_disp.fbink_state.current_rota;
      g_disp.info.has_color_panel = g_disp.fbink_state.has_color_panel;
      g_disp.info.is_mtk = g_disp.fbink_state.is_mtk;
      g_disp.info.touch_swap_axes = g_disp.fbink_state.touch_swap_axes;
      g_disp.info.touch_mirror_x = g_disp.fbink_state.touch_mirror_x;
      g_disp.info.touch_mirror_y = g_disp.fbink_state.touch_mirror_y;
      if (g_disp.fbink_state.device_name[0] != '\0') {
        std::snprintf(g_disp.info.device_name, sizeof(g_disp.info.device_name),
                      "Kobo %s (%s)", g_disp.fbink_state.device_name, g_disp.fbink_state.device_codename);
      }
      if (fbink_dump(g_disp.fbfd, &g_disp.nickel_dump) == 0) {
        g_disp.has_nickel_dump = true;
      }
    }
  }
#else
  g_disp.fbfd = ::open("/dev/fb0", O_RDWR);
  if (g_disp.fbfd >= 0) {
    struct fb_var_screeninfo vinfo {};
    struct fb_fix_screeninfo finfo {};
    if (::ioctl(g_disp.fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
        ::ioctl(g_disp.fbfd, FBIOGET_FSCREENINFO, &finfo) == 0) {
      g_disp.info.screen_width = vinfo.xres;
      g_disp.info.screen_height = vinfo.yres;
      g_disp.info.bpp = vinfo.bits_per_pixel;
      g_disp.info.stride_bytes = finfo.line_length;
      g_disp.info.current_rota = static_cast<uint8_t>(vinfo.rotate);
      g_disp.fb_mmap_size = finfo.smem_len;
      void* mapped = ::mmap(nullptr, g_disp.fb_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_disp.fbfd, 0);
      if (mapped != MAP_FAILED) {
        g_disp.fb_mmap = static_cast<uint8_t*>(mapped);
      }
    }
  }
#endif
#endif

  g_disp.surface_bgra.assign(
      static_cast<size_t>(g_disp.info.screen_width) * g_disp.info.screen_height, 0xFFFFFFFFU);
  g_disp.info.render_mode = KOBO_RENDER_QUALITY;
  kobo_display_set_layout(/*portrait=*/true, /*preferred_scale=*/6, out_info);
  g_disp.initialized = true;
  return true;
}

void kobo_display_shutdown(void) {
#if defined(__linux__)
#if defined(HAVE_FBINK)
  if (g_disp.fbfd >= 0) {
    // Let every queued EPDC update finish before touching the screen Nickel will own again
    fbink_wait_for_complete(g_disp.fbfd, LAST_MARKER);
    if (g_disp.has_nickel_dump) {
      FBInkConfig restore_cfg = g_disp.fbink_cfg;
      restore_cfg.wfm_mode = WFM_GC16;
      restore_cfg.is_flashing = true;
      fbink_restore(g_disp.fbfd, &restore_cfg, &g_disp.nickel_dump);
      fbink_wait_for_complete(g_disp.fbfd, LAST_MARKER);
      fbink_free_dump_data(&g_disp.nickel_dump);
      g_disp.has_nickel_dump = false;
    }
    fbink_close(g_disp.fbfd);
    g_disp.fbfd = -1;
  }
#else
  if (g_disp.fb_mmap && g_disp.fb_mmap_size > 0) {
    ::munmap(g_disp.fb_mmap, g_disp.fb_mmap_size);
    g_disp.fb_mmap = nullptr;
  }
  if (g_disp.fbfd >= 0) {
    ::close(g_disp.fbfd);
    g_disp.fbfd = -1;
  }
#endif
#endif
  g_disp.surface_bgra.clear();
  g_disp.rgba_staging.clear();
  g_disp.initialized = false;
}

void kobo_display_set_layout(bool portrait, int preferred_scale, kobo_display_info_t* out_info) {
  g_disp.info.portrait = portrait;
  const int sw = static_cast<int>(g_disp.info.screen_width);
  const int sh = static_cast<int>(g_disp.info.screen_height);

  int max_scale_w = sw / GB_LCD_WIDTH;
  int max_scale_h = (sh - 340) / GB_LCD_HEIGHT;
  int max_scale = std::max(1, std::min(max_scale_w, max_scale_h));

  int chosen_scale = preferred_scale > 0 ? std::min(preferred_scale, max_scale) : std::min(6, max_scale);
  if (chosen_scale < 1) chosen_scale = 1;

  g_disp.info.scale = chosen_scale;
  g_disp.info.game_w = GB_LCD_WIDTH * chosen_scale;
  g_disp.info.game_h = GB_LCD_HEIGHT * chosen_scale;
  g_disp.info.game_x = (sw - g_disp.info.game_w) / 2;
  // Align game_x to even pixels for Kaleido 3 2x2 CFA subpixel grid alignment
  g_disp.info.game_x &= ~1;
  g_disp.info.game_y = 56;  // Leave top header space for ROM title & FPS/Mode status bar
  g_disp.has_prev_frame = false;

  if (out_info) *out_info = g_disp.info;
}

void kobo_display_set_render_mode(kobo_render_mode_t mode) {
  if (g_disp.info.render_mode != mode) {
    g_disp.info.render_mode = mode;
    g_disp.has_prev_frame = false;
  }
}

kobo_render_mode_t kobo_display_get_render_mode(void) {
  return g_disp.info.render_mode;
}

bool kobo_display_compute_dirty_rect(const uint16_t* prev_rgb565,
                                     const uint16_t* curr_rgb565,
                                     const uint8_t* prev_shade,
                                     const uint8_t* curr_shade,
                                     bool compare_rgb,
                                     kobo_rect_t* out_game_rect) {
  constexpr int kTileSize = 8;
  constexpr int kTilesX = GB_LCD_WIDTH / kTileSize;   // 20
  constexpr int kTilesY = GB_LCD_HEIGHT / kTileSize;  // 18

  int min_tx = kTilesX, min_ty = kTilesY;
  int max_tx = -1, max_ty = -1;

  for (int ty = 0; ty < kTilesY; ++ty) {
    for (int tx = 0; tx < kTilesX; ++tx) {
      bool tile_dirty = false;
      for (int py = 0; py < kTileSize && !tile_dirty; ++py) {
        const int y = ty * kTileSize + py;
        const size_t row_idx = static_cast<size_t>(y) * GB_LCD_WIDTH + static_cast<size_t>(tx * kTileSize);
        if (compare_rgb && prev_rgb565 && curr_rgb565) {
          if (std::memcmp(prev_rgb565 + row_idx, curr_rgb565 + row_idx, kTileSize * sizeof(uint16_t)) != 0) {
            tile_dirty = true;
          }
        } else if (prev_shade && curr_shade) {
          if (std::memcmp(prev_shade + row_idx, curr_shade + row_idx, kTileSize) != 0) {
            tile_dirty = true;
          }
        }
      }
      if (tile_dirty) {
        if (tx < min_tx) min_tx = tx;
        if (ty < min_ty) min_ty = ty;
        if (tx > max_tx) max_tx = tx;
        if (ty > max_ty) max_ty = ty;
      }
    }
  }

  if (max_tx < min_tx || max_ty < min_ty) {
    return false;
  }

  if (out_game_rect) {
    out_game_rect->x = min_tx * kTileSize;
    out_game_rect->y = min_ty * kTileSize;
    out_game_rect->w = (max_tx - min_tx + 1) * kTileSize;
    out_game_rect->h = (max_ty - min_ty + 1) * kTileSize;
  }
  return true;
}

bool kobo_display_present_frame(const uint16_t* rgb565_frame,
                                const uint8_t* shade4_frame,
                                const uint8_t* bw_bayer_frame,
                                bool is_cgb_color,
                                bool force_full_clean) {
  if (!g_disp.initialized || !rgb565_frame || !shade4_frame || !bw_bayer_frame) return false;
  (void)is_cgb_color;

  const bool use_turbo_a2 = (g_disp.info.render_mode == KOBO_RENDER_TURBO_A2);
  const bool use_color = (!use_turbo_a2 && g_disp.info.has_color_panel);
  const uint8_t* active_mono = use_turbo_a2 ? bw_bayer_frame : shade4_frame;

  kobo_rect_t dirty_game {0, 0, GB_LCD_WIDTH, GB_LCD_HEIGHT};
  if (!force_full_clean && g_disp.has_prev_frame) {
    const bool changed = kobo_display_compute_dirty_rect(
        g_disp.prev_rgb565, rgb565_frame, g_disp.prev_shade, active_mono, use_color, &dirty_game);
    if (!changed) {
      ++g_disp.stats.frames_skipped_clean;
      return false;
    }
  }

  std::memcpy(g_disp.prev_rgb565, rgb565_frame, sizeof(g_disp.prev_rgb565));
  std::memcpy(g_disp.prev_shade, active_mono, sizeof(g_disp.prev_shade));
  g_disp.has_prev_frame = true;

  // 4-shade grayscale table for Kobo Clara HD (0=White, 1=Light Gray, 2=Dark Gray, 3=Black)
  static const uint32_t kGray4Bgra[4] = {
      0xFFFFFFFFU,
      0xFFAAAAAAU,
      0xFF555555U,
      0xFF000000U,
  };

  const int scale = g_disp.info.scale;
  const int gx0 = dirty_game.x;
  const int gy0 = dirty_game.y;
  const int gx1 = dirty_game.x + dirty_game.w;
  const int gy1 = dirty_game.y + dirty_game.h;

  for (int gy = gy0; gy < gy1; ++gy) {
    const size_t src_row = static_cast<size_t>(gy) * GB_LCD_WIDTH;
    for (int gx = gx0; gx < gx1; ++gx) {
      uint32_t pixel_bgra = 0xFFFFFFFFU;
      if (use_turbo_a2) {
        // 1-bit Bayer 2x2: bw_bayer_frame holds 0 (black) or 3 (white)
        pixel_bgra = (bw_bayer_frame[src_row + gx] == 0U) ? 0xFF000000U : 0xFFFFFFFFU;
      } else if (use_color) {
        // Kobo Libra Colour Kaleido 3 CFA native RGB565 -> BGRA8888 with chroma boost
        pixel_bgra = walnut_cgb_rgb565_to_kaleido3_bgra(rgb565_frame[src_row + gx], 1);
      } else {
        // Kobo Clara HD 4-level grayscale (WFM_DU4 / WFM_GC16)
        pixel_bgra = kGray4Bgra[shade4_frame[src_row + gx] & 0x03U];
      }

      const int dst_x = g_disp.info.game_x + gx * scale;
      const int dst_y = g_disp.info.game_y + gy * scale;
      fillRectSurface(dst_x, dst_y, scale, scale, pixel_bgra);
    }
  }

  kobo_rect_t screen_rect {
      g_disp.info.game_x + dirty_game.x * scale,
      g_disp.info.game_y + dirty_game.y * scale,
      dirty_game.w * scale,
      dirty_game.h * scale,
  };

  kobo_waveform_t wfm = KOBO_WFM_DU4;
  if (force_full_clean) {
    wfm = g_disp.info.has_color_panel ? KOBO_WFM_GCC16 : KOBO_WFM_GC16;
  } else if (use_turbo_a2) {
    wfm = KOBO_WFM_A2;
  } else if (use_color) {
    wfm = KOBO_WFM_GLRC16;
  }

  ++g_disp.stats.frames_presented;
  triggerEpdcRefresh(screen_rect, wfm, force_full_clean);
  return true;
}

void kobo_display_draw_hud(const char* title,
                           const char* status_line,
                           const kobo_hud_button_t* buttons,
                           size_t button_count,
                           bool full_flash) {
  if (!g_disp.initialized) return;

  const int sw = static_cast<int>(g_disp.info.screen_width);
  const int sh = static_cast<int>(g_disp.info.screen_height);

  if (full_flash) {
    fillRectSurface(0, 0, sw, sh, 0xFFFFFFFFU);
  } else {
    // Clear header & bottom control deck area
    fillRectSurface(0, 0, sw, g_disp.info.game_y - 4, 0xFFFFFFFFU);
    fillRectSurface(0, g_disp.info.game_y + g_disp.info.game_h + 4,
                    sw, sh - (g_disp.info.game_y + g_disp.info.game_h + 4), 0xFFFFFFFFU);
  }

  // Draw crisp 3px frame around the 160x144 scaled viewport
  drawRectBorder(g_disp.info.game_x - 3, g_disp.info.game_y - 3,
                 g_disp.info.game_w + 6, g_disp.info.game_h + 6, 3, 0xFF000000U);

  // Header bar: Title on the left, Device/Waveform status on the right
  if (title && *title) {
    drawTextScaled(g_disp.info.game_x, 18, title, 3, 0xFF000000U);
  }
  if (status_line && *status_line) {
    const int st_w = measureTextWidth(status_line, 2);
    drawTextScaled(std::max(16, g_disp.info.game_x + g_disp.info.game_w - st_w), 22,
                   status_line, 2, 0xFF333333U);
  }

  // Draw each virtual touch button
  for (size_t i = 0; i < button_count; ++i) {
    const kobo_hud_button_t& b = buttons[i];
    const uint32_t bg = b.pressed ? 0xFF000000U : 0xFFF4F4F4U;
    const uint32_t fg = b.pressed ? 0xFFFFFFFFU : 0xFF000000U;
    fillRectSurface(b.x, b.y, b.w, b.h, bg);
    drawRectBorder(b.x, b.y, b.w, b.h, 3, 0xFF000000U);

    if (b.label && *b.label) {
      const int scale = (std::strlen(b.label) <= 2) ? 4 : 2;
      const int tw = measureTextWidth(b.label, scale);
      const int tx = b.x + (b.w - tw) / 2;
      const int ty = b.y + (b.h - 8 * scale) / 2;
      drawTextScaled(tx, ty, b.label, scale, fg);
    }
  }

  kobo_rect_t full_rect {0, 0, sw, sh};
  const kobo_waveform_t wfm = full_flash
                                  ? (g_disp.info.has_color_panel ? KOBO_WFM_GCC16 : KOBO_WFM_GC16)
                                  : KOBO_WFM_DU;
  triggerEpdcRefresh(full_rect, wfm, full_flash);
}

size_t kobo_display_rom_selector_page_size(void) {
  const int usable = static_cast<int>(g_disp.info.screen_height) - KOBO_ROM_LIST_TOP - 220;
  return static_cast<size_t>(std::max(1, usable / KOBO_ROM_ITEM_H));
}

void kobo_display_draw_rom_selector(const char* const* rom_names,
                                    size_t rom_count,
                                    size_t selected_index,
                                    const char* device_banner,
                                    const char* footer_hint,
                                    const char* notice,
                                    bool full_flash) {
  if (!g_disp.initialized) return;
  const int sw = static_cast<int>(g_disp.info.screen_width);
  const int sh = static_cast<int>(g_disp.info.screen_height);

  fillRectSurface(0, 0, sw, sh, 0xFFFFFFFFU);

  // Top banner + prominent top-right [EXIT] touch button
  fillRectSurface(0, 0, sw, 110, 0xFF000000U);
  drawTextScaled(36, 26, "INK-BOY FOR KOBO", 4, 0xFFFFFFFFU);
  if (device_banner && *device_banner) {
    drawTextScaled(36, 74, device_banner, 2, 0xFFDDDDDDU);
  }
  fillRectSurface(sw - 240, 20, 204, 70, 0xFFFFFFFFU);
  drawRectBorder(sw - 240, 20, 204, 70, 3, 0xFF000000U);
  drawTextScaled(sw - 216, 43, "EXIT KOBO", 2, 0xFF000000U);

  if (notice && *notice) {
    drawTextScaled(40, 136, notice, 2, 0xFF000000U);
  } else {
    drawTextScaled(40, 136, "TAP A ROM OR USE BUTTONS TO PLAY (.GB / .GBC):", 2, 0xFF000000U);
  }

  const int list_top = KOBO_ROM_LIST_TOP;
  const int item_h = KOBO_ROM_ITEM_H;
  const size_t page_size = kobo_display_rom_selector_page_size();

  if (rom_count == 0) {
    drawRectBorder(40, list_top, sw - 80, 180, 3, 0xFF000000U);
    drawTextScaled(70, list_top + 45, "NO ROMS FOUND IN /MNT/ONBOARD/.ADDS/INKBOY/ROMS", 2, 0xFF000000U);
    drawTextScaled(70, list_top + 95, "TAP EXIT OR PRESS POWER BUTTON TO RETURN TO NICKEL", 2, 0xFF555555U);
  } else {
    const size_t page_start = (selected_index / page_size) * page_size;
    const size_t page_end = std::min(rom_count, page_start + page_size);
    for (size_t i = page_start; i < page_end; ++i) {
      const int y = list_top + static_cast<int>(i - page_start) * item_h;
      const bool sel = (i == selected_index);
      fillRectSurface(40, y, sw - 80, item_h - 12, sel ? 0xFF000000U : 0xFFF4F4F4U);
      drawRectBorder(40, y, sw - 80, item_h - 12, 3, 0xFF000000U);
      drawTextScaled(68, y + 26, rom_names[i], 3, sel ? 0xFFFFFFFFU : 0xFF000000U);
    }

    const size_t page_count = (rom_count + page_size - 1) / page_size;
    if (page_count > 1) {
      char page_label[32];
      std::snprintf(page_label, sizeof(page_label), "PAGE %zu/%zu",
                    selected_index / page_size + 1, page_count);
      drawTextScaled(sw - 40 - measureTextWidth(page_label, 2), sh - 180, page_label, 2, 0xFF000000U);
    }
  }

  // Bottom touch control bar for ROM selector (UP, DOWN, PLAY, EXIT)
  const int bar_y = sh - 136;
  const int bar_h = 96;
  const int btn_gap = 16;
  const int btn_w = (sw - 80 - btn_gap * 3) / 4;
  const char* labels[4] = {"UP", "DOWN", "PLAY", "EXIT"};
  for (int b = 0; b < 4; ++b) {
    const int bx = 40 + b * (btn_w + btn_gap);
    const bool accent = (b == 2);
    fillRectSurface(bx, bar_y, btn_w, bar_h, accent ? 0xFF000000U : 0xFFF4F4F4U);
    drawRectBorder(bx, bar_y, btn_w, bar_h, 3, 0xFF000000U);
    const int tw = measureTextWidth(labels[b], 3);
    drawTextScaled(bx + (btn_w - tw) / 2, bar_y + (bar_h - 24) / 2,
                   labels[b], 3, accent ? 0xFFFFFFFFU : 0xFF000000U);
  }

  if (footer_hint && *footer_hint) {
    drawTextScaled(40, sh - 28, footer_hint, 2, 0xFF333333U);
  }

  kobo_rect_t full_rect {0, 0, sw, sh};
  if (full_flash) {
    triggerEpdcRefresh(full_rect, g_disp.info.has_color_panel ? KOBO_WFM_GCC16 : KOBO_WFM_GC16, true);
  } else {
    // Menu navigation: grayscale-only UI, so a non-flashing GC16 keeps it fast without ghosting much
    triggerEpdcRefresh(full_rect, KOBO_WFM_GC16, false);
  }
}

void kobo_display_get_stats(kobo_display_stats_t* out_stats) {
  if (out_stats) *out_stats = g_disp.stats;
}

bool kobo_display_save_screenshot(const char* path) {
  if (!g_disp.initialized || !path) return false;
  FILE* fp = std::fopen(path, "wb");
  if (!fp) return false;

  const uint32_t w = g_disp.info.screen_width;
  const uint32_t h = g_disp.info.screen_height;
  std::fprintf(fp, "P6\n%u %u\n255\n", w, h);

  std::vector<uint8_t> rgb_row(static_cast<size_t>(w) * 3);
  for (uint32_t y = 0; y < h; ++y) {
    const uint32_t* src = &g_disp.surface_bgra[static_cast<size_t>(y) * w];
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t bgra = src[x];
      rgb_row[x * 3 + 0] = static_cast<uint8_t>((bgra >> 16) & 0xFF);
      rgb_row[x * 3 + 1] = static_cast<uint8_t>((bgra >> 8) & 0xFF);
      rgb_row[x * 3 + 2] = static_cast<uint8_t>(bgra & 0xFF);
    }
    std::fwrite(rgb_row.data(), 1, rgb_row.size(), fp);
  }
  std::fclose(fp);
  return true;
}

}  // extern "C"
