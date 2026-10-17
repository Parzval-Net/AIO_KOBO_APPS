#include "core/emulator_core.h"
#include "platform/kobo_display.h"
#include "platform/kobo_input.h"
#include "platform/kobo_system.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kFastDisplayPeriodUs = 45000ULL;     // ~22 FPS max in WFM_A2 Turbo 1-bit mode
constexpr uint64_t kQualityDisplayPeriodUs = 85000ULL;  // ~11.7 FPS max in WFM_GLRC16 / WFM_DU4 Quality mode
constexpr uint64_t kAutosavePeriodUs = 60000000ULL;     // Save battery-backed SRAM every 60 seconds
constexpr uint64_t kCleanRefreshPeriodUs = 60000000ULL; // Anti-ghosting full refresh every 60 seconds
constexpr size_t kMaxRoms = 64;

struct AppOptions {
  std::string rom_path;
  std::string roms_dir = "/mnt/onboard/.adds/inkboy/roms";
  std::string screenshot_path;
  kobo_device_type_t device_override = KOBO_DEVICE_UNKNOWN;
  kobo_render_mode_t initial_mode = KOBO_RENDER_QUALITY;
  libra_button_map_t libra_buttons = LIBRA_BTN_MAP_A_B;
  int scale = 6;
  int dmg_palette = -1;     // -1 = core default; 0..3 = DmgColorPalette from config.ini
  std::string config_path = "/mnt/onboard/.adds/inkboy/config.ini";
  uint32_t max_frames = 0;  // 0 = interactive infinite loop; >0 = run N frames and exit (for simulator/CI)
  bool suspend_nickel = true;
};

void printUsage(const char* prog) {
  std::printf(
      "Ink-boy for Kobo (Libra Colour & Clara HD) - Native E-Ink Game Boy / GBC Emulator\n"
      "Usage: %s [options] [rom.gb|rom.gbc]\n\n"
      "Options:\n"
      "  --device <libra|clara>     Force Kobo Libra Colour (Kaleido 3) or Kobo Clara HD (Carta)\n"
      "  --mode <quality|turbo>     Initial E-Ink waveform mode (quality=Color/4-Gray, turbo=1-bit A2)\n"
      "  --scale <6|7>              Integer scaling factor (default: 6 = 960x864, aligned for Kaleido 3 CFA)\n"
      "  --roms-dir <path>          Directory scanned by the E-Ink ROM selector\n"
      "  --frames <N>               Run N emulation frames and exit cleanly (for simulator/benchmarks)\n"
      "  --screenshot <file.ppm>    Export final rendered Kobo screen + HUD to a PPM image\n"
      "  --config <file.ini>        Settings file (default: /mnt/onboard/.adds/inkboy/config.ini)\n"
      "  --no-suspend-nickel        Do not send SIGSTOP/SIGCONT to nickel\n\n"
      "Command-line options override values from config.ini.\n",
      prog);
}

bool parseArgs(int argc, char** argv, AppOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      printUsage(argv[0]);
      return false;
    } else if (arg == "--device" && i + 1 < argc) {
      const std::string dev = argv[++i];
      if (dev == "clara" || dev == "clara_hd") {
        opts->device_override = KOBO_DEVICE_CLARA_HD;
      } else {
        opts->device_override = KOBO_DEVICE_LIBRA_COLOUR;
      }
    } else if (arg == "--mode" && i + 1 < argc) {
      const std::string m = argv[++i];
      opts->initial_mode = (m == "turbo" || m == "a2") ? KOBO_RENDER_TURBO_A2 : KOBO_RENDER_QUALITY;
    } else if (arg == "--scale" && i + 1 < argc) {
      opts->scale = std::atoi(argv[++i]);
    } else if (arg == "--roms-dir" && i + 1 < argc) {
      opts->roms_dir = argv[++i];
    } else if (arg == "--frames" && i + 1 < argc) {
      opts->max_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--screenshot" && i + 1 < argc) {
      opts->screenshot_path = argv[++i];
    } else if (arg == "--config" && i + 1 < argc) {
      ++i;  // Already consumed by loadConfigIni() before parseArgs()
    } else if (arg == "--no-suspend-nickel") {
      opts->suspend_nickel = false;
    } else if (!arg.empty() && arg[0] != '-') {
      opts->rom_path = arg;
    }
  }
  return true;
}

std::string trimCopy(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// Minimal INI reader for config.ini ([display] scale/mode/dmg_palette, [input] libra_buttons).
// A missing file is not an error: built-in defaults apply.
void loadConfigIni(const std::string& path, AppOptions* opts) {
  FILE* fp = std::fopen(path.c_str(), "r");
  if (!fp) return;
  char raw[256];
  while (std::fgets(raw, sizeof(raw), fp)) {
    std::string line = trimCopy(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trimCopy(line.substr(0, eq));
    const std::string val = trimCopy(line.substr(eq + 1));
    if (key == "scale") {
      const int v = std::atoi(val.c_str());
      if (v >= 1 && v <= 10) opts->scale = v;
    } else if (key == "mode") {
      if (val == "turbo" || val == "a2") opts->initial_mode = KOBO_RENDER_TURBO_A2;
      else if (val == "quality") opts->initial_mode = KOBO_RENDER_QUALITY;
    } else if (key == "dmg_palette") {
      const int v = std::atoi(val.c_str());
      if (!val.empty() && v >= 0 && v <= 3) opts->dmg_palette = v;
    } else if (key == "libra_buttons") {
      if (val == "A_B") opts->libra_buttons = LIBRA_BTN_MAP_A_B;
      else if (val == "B_A") opts->libra_buttons = LIBRA_BTN_MAP_B_A;
      else if (val == "START_SELECT") opts->libra_buttons = LIBRA_BTN_MAP_START_SELECT;
    }
  }
  std::fclose(fp);
  std::fprintf(stderr, "[Ink-boy] Loaded settings from %s\n", path.c_str());
}

std::string baseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string buildStatusString(const kobo_display_info_t& disp, const inkboy::EmulatorSession& session) {
  char buf[128];
  const char* mode_label =
      (disp.render_mode == KOBO_RENDER_TURBO_A2)
          ? "TURBO 1-BIT (WFM A2)"
          : (disp.has_color_panel ? "KALEIDO 3 COLOR (GLRC16)" : "CARTA 4-GRAY (DU4)");
  std::snprintf(buf, sizeof(buf), "%s / %s X%d",
                session.isCgbRom() ? "GBC" : "DMG", mode_label, disp.scale);
  return std::string(buf);
}

bool runRomSelector(const AppOptions& opts, const kobo_display_info_t& disp_info,
                    const std::string& notice, std::string* out_selected_rom) {
  static char rom_paths[kMaxRoms][512];
  static char rom_names[kMaxRoms][128];
  const size_t rom_count = kobo_system_scan_roms(opts.roms_dir.c_str(), rom_paths, rom_names, kMaxRoms);

  std::vector<const char*> name_ptrs(rom_count);
  for (size_t i = 0; i < rom_count; ++i) name_ptrs[i] = rom_names[i];

  const char* hint = "TAP A ROM TO PLAY  |  LIBRA BTNS: TOP=PLAY, BOT=NEXT  |  POWER/EXIT: QUIT";
  static size_t selected = 0;  // Remembered across returns from a game
  if (selected >= rom_count) selected = 0;
  const char* notice_c = notice.empty() ? nullptr : notice.c_str();
  auto redraw = [&](bool full_flash) {
    kobo_display_draw_rom_selector(name_ptrs.data(), rom_count, selected, disp_info.device_name, hint,
                                   notice_c, full_flash);
  };
  redraw(/*full_flash=*/true);

  if (opts.max_frames > 0) {
    if (rom_count > 0) {
      *out_selected_rom = rom_paths[0];
      return true;
    }
    return false;
  }

  const int sw = static_cast<int>(disp_info.screen_width);
  const int sh = static_cast<int>(disp_info.screen_height);
  const int list_top = KOBO_ROM_LIST_TOP;
  const int item_h = KOBO_ROM_ITEM_H;
  const size_t page_size = kobo_display_rom_selector_page_size();
  const int bar_y = sh - 136;
  const int bar_h = 96;
  const int btn_gap = 16;
  const int btn_w = (sw - 80 - btn_gap * 3) / 4;

  uint8_t prev_mask = 0;
  bool prev_touch = false;

  while (!kobo_system_should_quit()) {
    kobo_input_state_t in {};
    kobo_input_poll(&in, 16);

    if (in.ui_action == INKBOY_ACTION_EXIT) return false;

    // Handle direct touchscreen taps in the ROM selector
    const bool any_touch = (in.active_touch_count > 0);
    if (any_touch && !prev_touch) {
      for (int s = 0; s < KOBO_MAX_TOUCH_SLOTS; ++s) {
        if (!in.slots[s].active) continue;
        const int tx = in.slots[s].screen_x;
        const int ty = in.slots[s].screen_y;

        // Top-right [EXIT KOBO] button
        if (tx >= sw - 260 && ty <= 115) {
          return false;
        }

        // Bottom control bar: [UP] [DOWN] [PLAY] [EXIT]
        if (ty >= bar_y && ty <= bar_y + bar_h) {
          for (int b = 0; b < 4; ++b) {
            const int bx = 40 + b * (btn_w + btn_gap);
            if (tx >= bx && tx <= bx + btn_w) {
              if (b == 0 && rom_count > 0) {
                selected = (selected == 0) ? (rom_count - 1) : (selected - 1);
                redraw(/*full_flash=*/false);
              } else if (b == 1 && rom_count > 0) {
                selected = (selected + 1) % rom_count;
                redraw(/*full_flash=*/false);
              } else if (b == 2 && rom_count > 0) {
                *out_selected_rom = rom_paths[selected];
                return true;
              } else if (b == 3) {
                return false;
              }
            }
          }
        }

        // Tapping directly on a ROM row selects & launches it immediately
        if (rom_count > 0 && tx >= 36 && tx <= sw - 36 && ty >= list_top) {
          const size_t row = static_cast<size_t>((ty - list_top) / item_h);
          const size_t idx = (selected / page_size) * page_size + row;
          if (row < page_size && idx < rom_count) {
            selected = idx;
            *out_selected_rom = rom_paths[selected];
            return true;
          }
        }
      }
    }
    prev_touch = any_touch;

    const uint8_t newly_pressed = static_cast<uint8_t>(in.joypad_mask & ~prev_mask);
    prev_mask = in.joypad_mask;

    if ((newly_pressed & (INKBOY_PAD_UP | INKBOY_PAD_LEFT)) && rom_count > 0) {
      selected = (selected == 0) ? (rom_count - 1) : (selected - 1);
      redraw(/*full_flash=*/false);
    } else if ((newly_pressed & (INKBOY_PAD_DOWN | INKBOY_PAD_RIGHT | INKBOY_PAD_B)) && rom_count > 0) {
      selected = (selected + 1) % rom_count;
      redraw(/*full_flash=*/false);
    } else if ((newly_pressed & (INKBOY_PAD_A | INKBOY_PAD_START)) && rom_count > 0) {
      *out_selected_rom = rom_paths[selected];
      return true;
    } else if (rom_count == 0 && newly_pressed != 0) {
      return false;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  AppOptions opts;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--config") opts.config_path = argv[i + 1];
  }
  loadConfigIni(opts.config_path, &opts);
  if (!parseArgs(argc, argv, &opts)) {
    return 0;
  }

  kobo_system_config_t sys_cfg {};
  sys_cfg.suspend_nickel = opts.suspend_nickel;
  sys_cfg.base_dir = "/mnt/onboard/.adds/inkboy";
  sys_cfg.roms_dir = opts.roms_dir.c_str();
  kobo_system_init(&sys_cfg);

  kobo_display_info_t disp_info {};
  kobo_display_init(opts.device_override, &disp_info);
  kobo_display_set_render_mode(opts.initial_mode);
  kobo_display_set_layout(/*portrait=*/true, opts.scale, &disp_info);

  kobo_input_init(&disp_info, opts.libra_buttons);

  kobo_hud_button_t hud_buttons[16] = {};
  size_t hud_count = kobo_input_build_default_hud(&disp_info, hud_buttons, 16);
  kobo_input_set_touch_layout(&disp_info, hud_buttons, hud_count);

  std::string current_rom = opts.rom_path;
  std::string selector_notice;
  int dmg_palette = opts.dmg_palette;

  while (!kobo_system_should_quit()) {
    if (current_rom.empty()) {
      if (!runRomSelector(opts, disp_info, selector_notice, &current_rom)) {
        break;
      }
      selector_notice.clear();
    }

    inkboy::EmulatorSession session;
    std::string err;
    if (!session.loadRomFile(current_rom, &err)) {
      std::fprintf(stderr, "[Ink-boy] Failed to load ROM '%s': %s\n", current_rom.c_str(), err.c_str());
      if (opts.max_frames > 0) break;  // Non-interactive runs must not loop on a broken ROM
      selector_notice = "COULD NOT LOAD " + baseName(current_rom) + ": " + err;
      current_rom.clear();
      continue;  // Back to the ROM selector with the error shown
    }
    if (dmg_palette >= 0) {
      session.setDmgPalette(static_cast<inkboy::DmgColorPalette>(dmg_palette));
    }

    std::string status = buildStatusString(disp_info, session);
    kobo_display_draw_hud(session.romTitle(), status.c_str(), hud_buttons, hud_count, /*full_flash=*/true);

    // Deterministic bounded-frame execution mode (for macOS Simulator, CI & screenshot export)
    if (opts.max_frames > 0) {
      for (uint32_t f = 0; f < opts.max_frames; ++f) {
        session.stepFrame();
      }
      if (const inkboy::EmulatorFrame* frame = session.acquireLatestFrame()) {
        kobo_display_present_frame(frame->rgb565, frame->shade4, frame->bw_bayer,
                                   frame->is_cgb_color, /*force_full_clean=*/false);
        session.releaseAcquiredFrame();
      }
      session.saveCartRamAtomically();
      break;
    }

    // Multi-threaded 59.73 Hz emulation task + asynchronous E-Ink presenter loop
    std::atomic<bool> emu_running{true};
    std::thread emu_thread([&]() {
      uint64_t next_autosave_us = kobo_system_now_us() + kAutosavePeriodUs;
      while (emu_running.load(std::memory_order_acquire) && !kobo_system_should_quit()) {
        const uint64_t now_us = kobo_system_now_us();
        session.runUntil(now_us);
        if (now_us >= next_autosave_us) {
          next_autosave_us = now_us + kAutosavePeriodUs;
          session.saveCartRamAtomically();
        }
        kobo_system_sleep_us(1500);
      }
    });

    uint64_t next_display_us = kobo_system_now_us();
    uint64_t next_clean_us = next_display_us + kCleanRefreshPeriodUs;
    uint8_t prev_joypad = 0xFF;
    bool return_to_rom_menu = false;

    while (!kobo_system_should_quit()) {
      kobo_input_state_t in_state {};
      kobo_input_poll(&in_state, 2);
      session.setJoypadMask(in_state.joypad_mask);

      // Update visual pressed state on HUD buttons when touch/buttons change
      if (in_state.joypad_mask != prev_joypad) {
        prev_joypad = in_state.joypad_mask;
        for (size_t i = 0; i < hud_count; ++i) {
          uint8_t bit = 0;
          switch (hud_buttons[i].control_id) {
            case HUD_CTRL_UP:     bit = INKBOY_PAD_UP; break;
            case HUD_CTRL_DOWN:   bit = INKBOY_PAD_DOWN; break;
            case HUD_CTRL_LEFT:   bit = INKBOY_PAD_LEFT; break;
            case HUD_CTRL_RIGHT:  bit = INKBOY_PAD_RIGHT; break;
            case HUD_CTRL_A:      bit = INKBOY_PAD_A; break;
            case HUD_CTRL_B:      bit = INKBOY_PAD_B; break;
            case HUD_CTRL_SELECT: bit = INKBOY_PAD_SELECT; break;
            case HUD_CTRL_START:  bit = INKBOY_PAD_START; break;
            default:              bit = 0; break;
          }
          hud_buttons[i].pressed = (bit != 0) && ((in_state.joypad_mask & bit) != 0);
        }
      }

      if (in_state.ui_action == INKBOY_ACTION_EXIT) {
        kobo_system_request_quit();
        break;
      } else if (in_state.ui_action == INKBOY_ACTION_ROM_MENU) {
        return_to_rom_menu = true;
        break;
      } else if (in_state.ui_action == INKBOY_ACTION_TOGGLE_MODE) {
        const kobo_render_mode_t next_mode =
            (kobo_display_get_render_mode() == KOBO_RENDER_QUALITY)
                ? KOBO_RENDER_TURBO_A2
                : KOBO_RENDER_QUALITY;
        kobo_display_set_render_mode(next_mode);
        disp_info.render_mode = next_mode;
        status = buildStatusString(disp_info, session);
        kobo_display_draw_hud(session.romTitle(), status.c_str(), hud_buttons, hud_count, false);
      } else if (in_state.ui_action == INKBOY_ACTION_CYCLE_PALETTE) {
        session.cycleDmgPalette();
        dmg_palette = static_cast<int>(session.dmgPalette());  // Keep the choice for the next ROM
      } else if (in_state.ui_action == INKBOY_ACTION_CLEAN_SCREEN) {
        next_clean_us = 0;
      } else if (in_state.ui_action == INKBOY_ACTION_ROTATE) {
        kobo_display_set_layout(!disp_info.portrait, disp_info.scale, &disp_info);
        hud_count = kobo_input_build_default_hud(&disp_info, hud_buttons, 16);
        kobo_input_set_touch_layout(&disp_info, hud_buttons, hud_count);
        status = buildStatusString(disp_info, session);
        kobo_display_draw_hud(session.romTitle(), status.c_str(), hud_buttons, hud_count, true);
      }

      const uint64_t now_us = kobo_system_now_us();
      if (now_us >= next_display_us) {
        const bool force_clean = (now_us >= next_clean_us);
        if (force_clean) next_clean_us = now_us + kCleanRefreshPeriodUs;

        if (const inkboy::EmulatorFrame* frame = session.acquireLatestFrame()) {
          kobo_display_present_frame(frame->rgb565, frame->shade4, frame->bw_bayer,
                                     frame->is_cgb_color, force_clean);
          session.releaseAcquiredFrame();
        }

        const uint64_t period = (kobo_display_get_render_mode() == KOBO_RENDER_TURBO_A2)
                                    ? kFastDisplayPeriodUs
                                    : kQualityDisplayPeriodUs;
        next_display_us = now_us + period;
      }
    }

    emu_running.store(false, std::memory_order_release);
    if (emu_thread.joinable()) emu_thread.join();
    session.saveCartRamAtomically();

    if (return_to_rom_menu) {
      current_rom.clear();
      continue;
    }
    break;
  }

  if (!opts.screenshot_path.empty()) {
    kobo_display_save_screenshot(opts.screenshot_path.c_str());
  }

  // Orderly hand-back to Nickel: no held keys/fingers, screen restored and settled, then ungrab
  std::fprintf(stderr, "[Ink-boy] exit: waiting for keys and touches to be released\n");
  if (!kobo_input_wait_for_release(3000)) {
    std::fprintf(stderr, "[Ink-boy] exit: release wait timed out, continuing\n");
  }
  std::fprintf(stderr, "[Ink-boy] exit: restoring Nickel screen\n");
  kobo_display_shutdown();
  std::fprintf(stderr, "[Ink-boy] exit: releasing input devices\n");
  kobo_input_shutdown();
  kobo_system_shutdown();
  std::fprintf(stderr, "[Ink-boy] exit: done\n");
  return 0;
}
