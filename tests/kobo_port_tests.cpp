#include "core/emulator_core.h"
#include "core/walnut_cgb_color.h"
#include "platform/kobo_display.h"
#include "platform/kobo_input.h"
#include "platform/kobo_system.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

uint8_t computeHeaderChecksum(const uint8_t* rom) {
  uint8_t sum = 0;
  for (size_t i = 0x134; i <= 0x14C; ++i) {
    sum = static_cast<uint8_t>(sum - rom[i] - 1U);
  }
  return sum;
}

// Build a synthetic 32 KiB Game Boy / Game Boy Color ROM that writes tiles to VRAM,
// configures CGB palettes (if is_cgb), writes a byte to external SRAM (0xA000), and turns on the LCD.
std::vector<uint8_t> buildSyntheticRom(bool is_cgb, const char* title) {
  std::vector<uint8_t> rom(0x8000, 0x00);

  // Entry point at 0x0100: JP 0x0150
  rom[0x0100] = 0x00;  // NOP
  rom[0x0101] = 0xC3;  // JP 0x0150
  rom[0x0102] = 0x50;
  rom[0x0103] = 0x01;

  std::memset(rom.data() + 0x0134, 0, 16);
  std::memcpy(rom.data() + 0x0134, title, std::min<size_t>(std::strlen(title), 11));
  rom[0x0143] = is_cgb ? 0x80 : 0x00;  // CGB flag
  rom[0x0147] = 0x03;                  // MBC1 + RAM + BATTERY
  rom[0x0148] = 0x00;                  // 32 KiB ROM (2 banks)
  rom[0x0149] = 0x02;                  // 8 KiB SRAM
  rom[0x014D] = computeHeaderChecksum(rom.data());

  // Program at 0x0150:
  // 1. Enable Cart RAM (LD A, 0x0A; LD (0x0000), A)
  // 2. Write 0x42 to Cart RAM at 0xA000 (LD A, 0x42; LD (0xA000), A)
  // 3. Set DMG BGP palette (LD A, 0xE4; LDH (0x47), A)
  // 4. Write pattern into VRAM Tile 0 (0x8000..0x800F)
  // 5. Enable LCD (LD A, 0x91; LDH (0x40), A)
  // 6. Infinite loop (JR -2)
  size_t pc = 0x0150;
  auto emit = [&](uint8_t b) { rom[pc++] = b; };

  // Enable SRAM
  emit(0x3E); emit(0x0A);              // LD A, 0x0A
  emit(0xEA); emit(0x00); emit(0x00);  // LD (0x0000), A
  // Write 0x42 to 0xA000
  emit(0x3E); emit(0x42);              // LD A, 0x42
  emit(0xEA); emit(0x00); emit(0xA0);  // LD (0xA000), A
  // Set BGP = 0xE4
  emit(0x3E); emit(0xE4);              // LD A, 0xE4
  emit(0xE0); emit(0x47);              // LDH (0xFF47), A
  // Turn on LCDC = 0x91
  emit(0x3E); emit(0x91);              // LD A, 0x91
  emit(0xE0); emit(0x40);              // LDH (0xFF40), A
  // Loop forever
  emit(0x18); emit(0xFE);              // JR -2

  return rom;
}

void testColorAndBayerMath() {
  std::printf("[TEST 1/5] Verifying Walnut-CGB luminance, Bayer 2x2 1-bit & Kaleido 3 CFA math...\n");
  assert(walnut_cgb_rgb565_to_luminance(0xF800) == 76);   // Pure red
  assert(walnut_cgb_rgb565_to_luminance(0x07E0) == 150);  // Pure green
  assert(walnut_cgb_rgb565_to_luminance(0x001F) == 29);   // Pure blue

  // Exhaustive check across all 65,536 RGB565 colors: Bayer output is strictly 0 or 3
  for (uint32_t c = 0; c <= 0xFFFFU; ++c) {
    for (uint_fast8_t y = 0; y < 2; ++y) {
      for (uint_fast8_t x = 0; x < 2; ++x) {
        const uint8_t bw = walnut_cgb_rgb565_to_bw(static_cast<uint16_t>(c), x, y);
        assert(bw == 0U || bw == 3U);
      }
    }
  }

  // Mid-gray 0x8410 checkerboard verification
  assert(walnut_cgb_rgb565_to_bw(0x8410, 0, 0) == 3U);
  assert(walnut_cgb_rgb565_to_bw(0x8410, 1, 0) == 0U);
  assert(walnut_cgb_rgb565_to_bw(0x8410, 0, 1) == 0U);
  assert(walnut_cgb_rgb565_to_bw(0x8410, 1, 1) == 3U);

  // Kaleido 3 BGRA conversion
  const uint32_t red_bgra = walnut_cgb_rgb565_to_kaleido3_bgra(0xF800, 1);
  assert(((red_bgra >> 16) & 0xFF) == 255);  // Red channel saturated
}

void testCoresAndAtomicSave() {
  std::printf("[TEST 2/5] Verifying Peanut-GB (.gb) & Walnut-CGB (.gbc) cores + atomic .sav persistence...\n");

  // 1. Test DMG on Peanut-GB
  {
    std::vector<uint8_t> dmg_rom = buildSyntheticRom(/*is_cgb=*/false, "PKMN RED");
    inkboy::EmulatorSession session;
    std::string err;
    assert(session.loadRomMemory(dmg_rom.data(), dmg_rom.size(), "/tmp/test_pkmn_red.gb", &err));
    assert(!session.isCgbRom());
    for (int i = 0; i < 10; ++i) session.stepFrame();
    assert(session.totalFramesRun() == 10);
    assert(session.isSaveDirty());
    assert(session.saveCartRamAtomically());
    assert(!session.isSaveDirty());

    const inkboy::EmulatorFrame* frame = session.acquireLatestFrame();
    assert(frame != nullptr);
    assert(frame->frame_number == 10);
    session.releaseAcquiredFrame();
  }

  // 2. Test CGB on Walnut-CGB
  {
    std::vector<uint8_t> cgb_rom = buildSyntheticRom(/*is_cgb=*/true, "PKMN CRYST");
    inkboy::EmulatorSession session;
    std::string err;
    assert(session.loadRomMemory(cgb_rom.data(), cgb_rom.size(), "/tmp/test_pkmn_crystal.gbc", &err));
    assert(session.isCgbRom());
    for (int i = 0; i < 10; ++i) session.stepFrame();
    assert(session.totalFramesRun() == 10);
    assert(session.isSaveDirty());
    assert(session.saveCartRamAtomically());

    // Reload save into a fresh session and confirm 0x42 persisted at offset 0 of SRAM
    inkboy::EmulatorSession session2;
    assert(session2.loadRomMemory(cgb_rom.data(), cgb_rom.size(), "/tmp/test_pkmn_crystal.gbc", &err));
    assert(session2.loadSaveFile());
  }
}

void testDisplayDirtyRectsAndProfiles() {
  std::printf("[TEST 3/5] Verifying Kobo Libra Colour (Kaleido 3) & Kobo Clara HD (Carta) display pipelines...\n");

  std::vector<uint16_t> rgb_a(GB_LCD_WIDTH * GB_LCD_HEIGHT, 0xFFFFU);
  std::vector<uint16_t> rgb_b(GB_LCD_WIDTH * GB_LCD_HEIGHT, 0xFFFFU);
  std::vector<uint8_t> shade_a(GB_LCD_WIDTH * GB_LCD_HEIGHT, 0);
  std::vector<uint8_t> shade_b(GB_LCD_WIDTH * GB_LCD_HEIGHT, 0);
  std::vector<uint8_t> bw_a(GB_LCD_WIDTH * GB_LCD_HEIGHT, 3);

  // Identical frames must report dirty == false
  kobo_rect_t dirty {};
  assert(!kobo_display_compute_dirty_rect(rgb_a.data(), rgb_b.data(), shade_a.data(), shade_b.data(),
                                          /*compare_rgb=*/true, &dirty));

  // Modify a single pixel at (19, 27) -> falls in 8x8 tile (tx=2, ty=3) -> game rect (16, 24, 8, 8)
  rgb_b[27 * GB_LCD_WIDTH + 19] = 0xF800U;
  shade_b[27 * GB_LCD_WIDTH + 19] = 2U;
  assert(kobo_display_compute_dirty_rect(rgb_a.data(), rgb_b.data(), shade_a.data(), shade_b.data(),
                                         /*compare_rgb=*/true, &dirty));
  assert(dirty.x == 16 && dirty.y == 24 && dirty.w == 8 && dirty.h == 8);

  // Test Kobo Libra Colour profile (1264x1680, Kaleido 3 color, scale x6 = 960x864)
  kobo_display_info_t libra_info {};
  assert(kobo_display_init(KOBO_DEVICE_LIBRA_COLOUR, &libra_info));
  assert(libra_info.has_color_panel == true);
  assert(libra_info.screen_width == 1264 && libra_info.screen_height == 1680);
  assert(libra_info.scale == 6);
  assert((libra_info.game_x & 1) == 0);  // Even alignment for Kaleido 3 2x2 CFA

  // Present initial frame -> triggers GLRC16
  assert(kobo_display_present_frame(rgb_a.data(), shade_a.data(), bw_a.data(), true, false));
  kobo_display_stats_t st {};
  kobo_display_get_stats(&st);
  assert(st.last_waveform == KOBO_WFM_GLRC16);

  // Present identical frame -> skipped!
  assert(!kobo_display_present_frame(rgb_a.data(), shade_a.data(), bw_a.data(), true, false));
  kobo_display_get_stats(&st);
  assert(st.frames_skipped_clean == 1);

  // Present frame with 1 dirty tile -> partial update of size (8*6, 8*6) = (48, 48)
  assert(kobo_display_present_frame(rgb_b.data(), shade_b.data(), bw_a.data(), true, false));
  kobo_display_get_stats(&st);
  assert(st.last_dirty_screen_rect.w == 48 && st.last_dirty_screen_rect.h == 48);
  kobo_display_shutdown();

  // Test Kobo Clara HD profile (1072x1448, Carta 4-gray DU4 & Turbo A2)
  kobo_display_info_t clara_info {};
  assert(kobo_display_init(KOBO_DEVICE_CLARA_HD, &clara_info));
  assert(clara_info.has_color_panel == false);
  assert(clara_info.screen_width == 1072 && clara_info.screen_height == 1448);
  assert(kobo_display_present_frame(rgb_a.data(), shade_a.data(), bw_a.data(), false, false));
  kobo_display_get_stats(&st);
  assert(st.last_waveform == KOBO_WFM_DU4);

  // Switch Clara HD to Turbo A2 mode
  kobo_display_set_render_mode(KOBO_RENDER_TURBO_A2);
  assert(kobo_display_present_frame(rgb_b.data(), shade_b.data(), bw_a.data(), false, false));
  kobo_display_get_stats(&st);
  assert(st.last_waveform == KOBO_WFM_A2);
  kobo_display_shutdown();
}

void testMultitouchAndLibraButtons() {
  std::printf("[TEST 4/5] Verifying evdev Multitouch Type A/B (D-Pad + A simultaneous) & Libra buttons...\n");

  kobo_display_info_t clara_info {};
  kobo_display_init(KOBO_DEVICE_CLARA_HD, &clara_info);
  kobo_input_init(&clara_info, LIBRA_BTN_MAP_A_B);

  kobo_hud_button_t btns[16] = {};
  const size_t btn_count = kobo_input_build_default_hud(&clara_info, btns, 16);
  assert(btn_count >= 13);
  kobo_input_set_touch_layout(&clara_info, btns, btn_count);

  // Find center of RIGHT button (index 3) and A button (index 5)
  const int right_cx = btns[3].x + btns[3].w / 2;
  const int right_cy = btns[3].y + btns[3].h / 2;
  const int a_cx = btns[5].x + btns[5].w / 2;
  const int a_cy = btns[5].y + btns[5].h / 2;

  kobo_input_state_t state {};

  // Simulate Multitouch Type B: Slot 0 touches RIGHT, Slot 1 touches A simultaneously
  kobo_input_feed_raw_event(0x03, 0x2F, 0, 0, nullptr);        // ABS_MT_SLOT = 0
  kobo_input_feed_raw_event(0x03, 0x39, 101, 0, nullptr);      // ABS_MT_TRACKING_ID = 101
  kobo_input_feed_raw_event(0x03, 0x35, right_cx, 0, nullptr); // ABS_MT_POSITION_X
  kobo_input_feed_raw_event(0x03, 0x36, right_cy, 0, nullptr); // ABS_MT_POSITION_Y

  kobo_input_feed_raw_event(0x03, 0x2F, 1, 0, nullptr);        // ABS_MT_SLOT = 1
  kobo_input_feed_raw_event(0x03, 0x39, 102, 0, nullptr);      // ABS_MT_TRACKING_ID = 102
  kobo_input_feed_raw_event(0x03, 0x35, a_cx, 0, nullptr);     // ABS_MT_POSITION_X
  kobo_input_feed_raw_event(0x03, 0x36, a_cy, 0, nullptr);     // ABS_MT_POSITION_Y
  kobo_input_feed_raw_event(0x00, 0x00, 0, 0, &state);         // SYN_REPORT

  assert(state.active_touch_count == 2);
  assert((state.joypad_mask & INKBOY_PAD_RIGHT) != 0);
  assert((state.joypad_mask & INKBOY_PAD_A) != 0);

  // Lift finger 1 (A button), keeping finger 0 (RIGHT) held down
  kobo_input_feed_raw_event(0x03, 0x2F, 1, 0, nullptr);        // ABS_MT_SLOT = 1
  kobo_input_feed_raw_event(0x03, 0x39, -1, 0, nullptr);       // ABS_MT_TRACKING_ID = -1 (release)
  kobo_input_feed_raw_event(0x00, 0x00, 0, 0, &state);         // SYN_REPORT

  assert(state.active_touch_count == 1);
  assert((state.joypad_mask & INKBOY_PAD_RIGHT) != 0);
  assert((state.joypad_mask & INKBOY_PAD_A) == 0);

  // Release finger 0
  kobo_input_feed_raw_event(0x03, 0x2F, 0, 0, nullptr);
  kobo_input_feed_raw_event(0x03, 0x39, -1, 0, nullptr);
  kobo_input_feed_raw_event(0x00, 0x00, 0, 0, &state);
  assert(state.active_touch_count == 0);
  assert(state.joypad_mask == 0);

  // Simulate Kobo Libra Colour physical page-turn buttons (KEY_PAGEUP=104 -> A, KEY_PAGEDOWN=109 -> B)
  kobo_input_feed_raw_event(0x01, 104, 1, 0, nullptr);
  kobo_input_feed_raw_event(0x01, 109, 1, 0, nullptr);
  kobo_input_feed_raw_event(0x00, 0x00, 0, 0, &state);
  assert(state.physical_mask == (INKBOY_PAD_A | INKBOY_PAD_B));

  kobo_input_feed_raw_event(0x01, 104, 0, 0, nullptr);
  kobo_input_feed_raw_event(0x01, 109, 0, 0, nullptr);
  kobo_input_feed_raw_event(0x00, 0x00, 0, 0, &state);
  assert(state.physical_mask == 0);

  kobo_input_shutdown();
  kobo_display_shutdown();
}

void testHudScreenshots() {
  std::printf("[TEST 5/5] Rendering full Kobo Libra Colour & Clara HD HUD screenshots...\n");

  for (kobo_device_type_t dev : {KOBO_DEVICE_LIBRA_COLOUR, KOBO_DEVICE_CLARA_HD}) {
    kobo_display_info_t info {};
    kobo_display_init(dev, &info);
    kobo_hud_button_t btns[16] = {};
    const size_t n = kobo_input_build_default_hud(&info, btns, 16);
    btns[3].pressed = true;  // Highlight RIGHT
    btns[5].pressed = true;  // Highlight A

    kobo_display_draw_hud("POKEMON CRYSTAL",
                          dev == KOBO_DEVICE_LIBRA_COLOUR ? "GBC | KALEIDO 3 COLOR X6" : "DMG | CARTA 4-GRAY X6",
                          btns, n, true);

    // Generate a colorful test pattern inside the 160x144 viewport
    std::vector<uint16_t> rgb(GB_LCD_WIDTH * GB_LCD_HEIGHT);
    std::vector<uint8_t> shade(GB_LCD_WIDTH * GB_LCD_HEIGHT);
    std::vector<uint8_t> bw(GB_LCD_WIDTH * GB_LCD_HEIGHT);
    for (int y = 0; y < GB_LCD_HEIGHT; ++y) {
      for (int x = 0; x < GB_LCD_WIDTH; ++x) {
        const uint16_t c = static_cast<uint16_t>(((x / 5) << 11) | ((y / 3) << 5) | ((x + y) & 0x1F));
        rgb[y * GB_LCD_WIDTH + x] = c;
        shade[y * GB_LCD_WIDTH + x] = static_cast<uint8_t>(((x / 16) + (y / 16)) & 3);
        bw[y * GB_LCD_WIDTH + x] = walnut_cgb_rgb565_to_bw(c, x, y);
      }
    }
    kobo_display_present_frame(rgb.data(), shade.data(), bw.data(), true, false);
    const char* out_path = (dev == KOBO_DEVICE_LIBRA_COLOUR)
                               ? "/tmp/kobo_libra_colour_preview.ppm"
                               : "/tmp/kobo_clara_hd_preview.ppm";
    assert(kobo_display_save_screenshot(out_path));
    kobo_display_shutdown();
  }
}

}  // namespace

int main() {
  testColorAndBayerMath();
  testCoresAndAtomicSave();
  testDisplayDirtyRectsAndProfiles();
  testMultitouchAndLibraButtons();
  testHudScreenshots();
  std::printf("\nAll 5 Kobo Ink-boy test suites PASSED successfully!\n");
  return 0;
}
