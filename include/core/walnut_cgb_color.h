#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert the RGB565 palette values used by Walnut-CGB to display luminance (0..255). */
static inline uint8_t walnut_cgb_rgb565_to_luminance(uint16_t color) {
  const uint8_t red5 = (uint8_t)((color >> 11) & 0x1FU);
  const uint8_t green6 = (uint8_t)((color >> 5) & 0x3FU);
  const uint8_t blue5 = (uint8_t)(color & 0x1FU);
  const uint16_t red = (uint16_t)((red5 << 3) | (red5 >> 2));
  const uint16_t green = (uint16_t)((green6 << 2) | (green6 >> 4));
  const uint16_t blue = (uint16_t)((blue5 << 3) | (blue5 >> 2));
  return (uint8_t)((299U * red + 587U * green + 114U * blue + 500U) / 1000U);
}

/*
 * Ordered 2x2 Bayer dithering from Ink-boy.
 * Outputs strictly 0 (black) or 3 (white) in Ink-boy's Walnut-CGB convention,
 * used by the 1-bit WFM_A2 / WFM_DU turbo mode on both Kobo Clara HD and Libra Colour.
 */
static inline uint8_t walnut_cgb_luminance_to_bw(uint8_t luminance, uint_fast8_t x, uint_fast8_t y) {
  static const uint8_t bayer[2][2] = {{0, 2}, {3, 1}};
  const uint16_t threshold = (uint16_t)(((uint16_t)(2U * bayer[y & 1U][x & 1U] + 1U) * 255U) / 8U);
  return luminance > threshold ? 3U : 0U;
}

static inline uint8_t walnut_cgb_rgb565_to_bw(uint16_t color, uint_fast8_t x, uint_fast8_t y) {
  return walnut_cgb_luminance_to_bw(walnut_cgb_rgb565_to_luminance(color), x, y);
}

/*
 * Quantize 8-bit luminance (0=black..255=white) to 4 Game Boy shades:
 * 0 = lightest (white), 1 = light gray, 2 = dark gray, 3 = darkest (black).
 * Used on Kobo Clara HD in 4-gray quality mode (WFM_DU4 / WFM_GC16).
 */
static inline uint8_t walnut_cgb_luminance_to_shade4(uint8_t luminance) {
  if (luminance >= 192U) return 0U;
  if (luminance >= 128U) return 1U;
  if (luminance >= 64U) return 2U;
  return 3U;
}

/*
 * Convert RGB565 to 32-bit BGRA8888 with optional saturation boost for Kaleido 3 CFA
 * on Kobo Libra Colour. Kaleido 3's passive color filter array attenuates chroma,
 * so a subtle 1.25x chroma boost improves legibility of Game Boy Color sprites.
 */
static inline uint32_t walnut_cgb_rgb565_to_kaleido3_bgra(uint16_t color, int boost_chroma) {
  const uint8_t r5 = (uint8_t)((color >> 11) & 0x1FU);
  const uint8_t g6 = (uint8_t)((color >> 5) & 0x3FU);
  const uint8_t b5 = (uint8_t)(color & 0x1FU);
  int r = (r5 << 3) | (r5 >> 2);
  int g = (g6 << 2) | (g6 >> 4);
  int b = (b5 << 3) | (b5 >> 2);

  if (boost_chroma) {
    const int luma = (299 * r + 587 * g + 114 * b) / 1000;
    r = luma + ((r - luma) * 5) / 4;
    g = luma + ((g - luma) * 5) / 4;
    b = luma + ((b - luma) * 5) / 4;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
  }

  return (0xFF000000U) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#ifdef __cplusplus
}
#endif
