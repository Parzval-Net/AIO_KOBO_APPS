#include "platform/kobo_input.h"

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

/* Linux input event constants defined portably so unit tests on macOS can also feed raw events */
constexpr uint16_t kEvSyn = 0x00;
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;

constexpr uint16_t kSynReport = 0x00;
constexpr uint16_t kSynMtReport = 0x02;

constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kAbsHat0X = 0x10;
constexpr uint16_t kAbsHat0Y = 0x11;
constexpr uint16_t kAbsMtSlot = 0x2F;
constexpr uint16_t kAbsMtPositionX = 0x35;
constexpr uint16_t kAbsMtPositionY = 0x36;
constexpr uint16_t kAbsMtTrackingId = 0x39;

constexpr uint16_t kBtnTouch = 0x14A;

/* Kobo Libra Colour / Kobo physical page-turn & power keycodes + standard gamepad/keyboard codes */
constexpr uint16_t kKeyEsc = 1;
constexpr uint16_t kKeyHome = 102;
constexpr uint16_t kKeyUp = 103;
constexpr uint16_t kKeyLeft = 105;
constexpr uint16_t kKeyRight = 106;
constexpr uint16_t kKeyDown = 108;
constexpr uint16_t kKeyPageUp = 104;
constexpr uint16_t kKeyPageDown = 109;
constexpr uint16_t kKeyPower = 116;     // Kobo physical power button -> immediate clean exit to Nickel
constexpr uint16_t kKeyEnter = 28;
constexpr uint16_t kKeySpace = 57;
constexpr uint16_t kKeyZ = 44;
constexpr uint16_t kKeyX = 45;
constexpr uint16_t kKeyF23 = 193;       // Kobo Libra Colour top page-turn button (RPgBack)
constexpr uint16_t kKeyF24 = 194;       // Kobo Libra Colour bottom page-turn button (RPgFwd)
constexpr uint16_t kBtnSouth = 0x130;   // Gamepad A
constexpr uint16_t kBtnEast = 0x131;    // Gamepad B
constexpr uint16_t kBtnSelect = 0x13A;  // Gamepad Select
constexpr uint16_t kBtnStart = 0x13B;   // Gamepad Start
constexpr uint16_t kBtnDpadUp = 0x220;
constexpr uint16_t kBtnDpadDown = 0x221;
constexpr uint16_t kBtnDpadLeft = 0x222;
constexpr uint16_t kBtnDpadRight = 0x223;

struct InputContext {
  bool initialized = false;
  kobo_display_info_t disp_info = {};
  libra_button_map_t libra_map = LIBRA_BTN_MAP_A_B;

  std::vector<kobo_hud_button_t> hud_buttons;

  // Multitouch Type A & Type B state
  int current_slot = 0;
  bool protocol_type_a_pending = false;
  int type_a_slot_cursor = 0;
  int last_device_kind = 0;
  kobo_touch_slot_t slots[KOBO_MAX_TOUCH_SLOTS] = {};

  std::bitset<0x300> held_keys;  // Every EV_KEY currently held, including power and BTN_TOUCH
  uint8_t physical_mask = 0;
  uint8_t touch_mask = 0;
  uint8_t prev_ui_ctrl_mask = 0;
  inkboy_ui_action_t pending_ui_action = INKBOY_ACTION_NONE;

#if defined(__linux__)
  int epoll_fd = -1;
  std::vector<int> device_fds;
#endif
};

InputContext g_input;

bool hitRect(int x, int y, const kobo_hud_button_t& b) {
  return x >= b.x && x < (b.x + b.w) && y >= b.y && y < (b.y + b.h);
}

uint8_t padBitForControlId(uint8_t ctrl_id) {
  switch (ctrl_id) {
    case HUD_CTRL_UP:     return INKBOY_PAD_UP;
    case HUD_CTRL_DOWN:   return INKBOY_PAD_DOWN;
    case HUD_CTRL_LEFT:   return INKBOY_PAD_LEFT;
    case HUD_CTRL_RIGHT:  return INKBOY_PAD_RIGHT;
    case HUD_CTRL_A:      return INKBOY_PAD_A;
    case HUD_CTRL_B:      return INKBOY_PAD_B;
    case HUD_CTRL_SELECT: return INKBOY_PAD_SELECT;
    case HUD_CTRL_START:  return INKBOY_PAD_START;
    default:              return 0;
  }
}

inkboy_ui_action_t uiActionForControlId(uint8_t ctrl_id) {
  switch (ctrl_id) {
    case HUD_CTRL_MODE:    return INKBOY_ACTION_TOGGLE_MODE;
    case HUD_CTRL_PALETTE: return INKBOY_ACTION_CYCLE_PALETTE;
    case HUD_CTRL_ROTATE:  return INKBOY_ACTION_ROTATE;
    case HUD_CTRL_CLEAN:   return INKBOY_ACTION_CLEAN_SCREEN;
    case HUD_CTRL_ROMS:    return INKBOY_ACTION_ROM_MENU;
    case HUD_CTRL_EXIT:    return INKBOY_ACTION_EXIT;
    default:               return INKBOY_ACTION_NONE;
  }
}

void transformTouchCoords(int raw_x, int raw_y, int device_kind, int* out_x, int* out_y) {
  const int sw = static_cast<int>(g_input.disp_info.screen_width);
  const int sh = static_cast<int>(g_input.disp_info.screen_height);
  int tx = raw_x;
  int ty = raw_y;

  // On real Kobo hardware digitizers (device_kind == 1), apply FBInk/KOReader axis swap & mirror quirks
  if (device_kind == 1) {
    if (g_input.disp_info.touch_swap_axes) {
      std::swap(tx, ty);
    }
    if (g_input.disp_info.touch_mirror_x) {
      tx = (sw - 1) - tx;
    }
    if (g_input.disp_info.touch_mirror_y) {
      ty = (sh - 1) - ty;
    }
  }

  *out_x = std::max(0, std::min(sw - 1, tx));
  *out_y = std::max(0, std::min(sh - 1, ty));
}

void evaluateTouchSlots() {
  uint8_t new_touch_mask = 0;
  uint8_t current_ui_ctrl_mask = 0;

  for (int s = 0; s < KOBO_MAX_TOUCH_SLOTS; ++s) {
    if (!g_input.slots[s].active) continue;
    transformTouchCoords(g_input.slots[s].raw_x, g_input.slots[s].raw_y,
                         g_input.last_device_kind,
                         &g_input.slots[s].screen_x, &g_input.slots[s].screen_y);
    const int sx = g_input.slots[s].screen_x;
    const int sy = g_input.slots[s].screen_y;

    for (const auto& btn : g_input.hud_buttons) {
      if (hitRect(sx, sy, btn)) {
        const uint8_t pad_bit = padBitForControlId(btn.control_id);
        if (pad_bit != 0) {
          new_touch_mask |= pad_bit;
        } else {
          const inkboy_ui_action_t act = uiActionForControlId(btn.control_id);
          if (act != INKBOY_ACTION_NONE) {
            const uint8_t bit = static_cast<uint8_t>(1u << (static_cast<uint8_t>(act) & 7u));
            current_ui_ctrl_mask |= bit;
            if ((g_input.prev_ui_ctrl_mask & bit) == 0) {
              g_input.pending_ui_action = act;
            }
          }
        }
      }
    }
  }

  g_input.touch_mask = new_touch_mask;
  g_input.prev_ui_ctrl_mask = current_ui_ctrl_mask;
}

uint8_t mapKeycodeToPadBit(uint16_t code) {
  // Kobo Libra Colour physical page-turn buttons (PageUp/F23 = Top button, PageDown/F24 = Bottom button)
  if (code == kKeyPageUp || code == kKeyF23) {
    switch (g_input.libra_map) {
      case LIBRA_BTN_MAP_A_B:          return INKBOY_PAD_A;
      case LIBRA_BTN_MAP_B_A:          return INKBOY_PAD_B;
      case LIBRA_BTN_MAP_START_SELECT: return INKBOY_PAD_START;
    }
  }
  if (code == kKeyPageDown || code == kKeyF24) {
    switch (g_input.libra_map) {
      case LIBRA_BTN_MAP_A_B:          return INKBOY_PAD_B;
      case LIBRA_BTN_MAP_B_A:          return INKBOY_PAD_A;
      case LIBRA_BTN_MAP_START_SELECT: return INKBOY_PAD_SELECT;
    }
  }

  switch (code) {
    case kKeyUp:
    case kBtnDpadUp:    return INKBOY_PAD_UP;
    case kKeyDown:
    case kBtnDpadDown:  return INKBOY_PAD_DOWN;
    case kKeyLeft:
    case kBtnDpadLeft:  return INKBOY_PAD_LEFT;
    case kKeyRight:
    case kBtnDpadRight: return INKBOY_PAD_RIGHT;
    case kBtnSouth:
    case kKeyZ:         return INKBOY_PAD_A;
    case kBtnEast:
    case kKeyX:         return INKBOY_PAD_B;
    case kBtnStart:
    case kKeyEnter:     return INKBOY_PAD_START;
    case kBtnSelect:
    case kKeySpace:     return INKBOY_PAD_SELECT;
    default:            return 0;
  }
}

void syncStateOutput(kobo_input_state_t* out_state) {
  if (!out_state) return;
  out_state->physical_mask = g_input.physical_mask;
  out_state->touch_mask = g_input.touch_mask;
  out_state->joypad_mask = static_cast<uint8_t>(g_input.physical_mask | g_input.touch_mask);
  out_state->ui_action = g_input.pending_ui_action;
  g_input.pending_ui_action = INKBOY_ACTION_NONE;

  int active_cnt = 0;
  for (int i = 0; i < KOBO_MAX_TOUCH_SLOTS; ++i) {
    out_state->slots[i] = g_input.slots[i];
    if (g_input.slots[i].active) ++active_cnt;
  }
  out_state->active_touch_count = active_cnt;
}

}  // namespace

extern "C" {

bool kobo_input_init(const kobo_display_info_t* display_info, libra_button_map_t btn_map) {
  kobo_input_shutdown();
  g_input = InputContext{};
  if (display_info) g_input.disp_info = *display_info;
  g_input.libra_map = btn_map;
  for (int i = 0; i < KOBO_MAX_TOUCH_SLOTS; ++i) {
    g_input.slots[i].tracking_id = -1;
  }

#if defined(__linux__)
  g_input.epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
  kobo_input_rescan_devices();
#endif

  g_input.initialized = true;
  return true;
}

void kobo_input_shutdown(void) {
#if defined(__linux__)
  for (int fd : g_input.device_fds) {
    if (fd >= 0) {
      ::ioctl(fd, EVIOCGRAB, 0);
      ::close(fd);
    }
  }
  g_input.device_fds.clear();
  if (g_input.epoll_fd >= 0) {
    ::close(g_input.epoll_fd);
    g_input.epoll_fd = -1;
  }
#endif
  g_input.initialized = false;
}

size_t kobo_input_build_default_hud(const kobo_display_info_t* display_info,
                                    kobo_hud_button_t* out_buttons,
                                    size_t max_buttons) {
  if (!display_info || !out_buttons || max_buttons < 13) return 0;

  const int sw = static_cast<int>(display_info->screen_width);
  const int sh = static_cast<int>(display_info->screen_height);
  const int deck_top = display_info->game_y + display_info->game_h + 24;
  const int deck_h = sh - deck_top - 20;

  // D-Pad cluster on the bottom-left (sized generously for blind thumb operation on Clara HD & Libra)
  const int dpad_size = std::min(124, std::max(90, deck_h / 4));
  const int dpad_cx = 44 + dpad_size;
  const int dpad_cy = deck_top + dpad_size + 12;

  size_t n = 0;
  out_buttons[n++] = {dpad_cx, dpad_cy - dpad_size, dpad_size, dpad_size, "UP", HUD_CTRL_UP, false};
  out_buttons[n++] = {dpad_cx, dpad_cy + dpad_size, dpad_size, dpad_size, "DN", HUD_CTRL_DOWN, false};
  out_buttons[n++] = {dpad_cx - dpad_size, dpad_cy, dpad_size, dpad_size, "LT", HUD_CTRL_LEFT, false};
  out_buttons[n++] = {dpad_cx + dpad_size, dpad_cy, dpad_size, dpad_size, "RT", HUD_CTRL_RIGHT, false};

  // Large B and A action buttons on the bottom-right (classic Game Boy diagonal offset)
  const int ab_w = std::min(156, std::max(116, sw / 7));
  const int ab_h = ab_w;
  const int a_x = sw - 44 - ab_w;
  const int a_y = dpad_cy - ab_h / 3;
  const int b_x = a_x - ab_w - 24;
  const int b_y = dpad_cy + ab_h / 3;

  out_buttons[n++] = {b_x, b_y, ab_w, ab_h, "B", HUD_CTRL_B, false};
  out_buttons[n++] = {a_x, a_y, ab_w, ab_h, "A", HUD_CTRL_A, false};

  // SELECT and START centered above the utility bar
  const int sel_w = 136;
  const int sel_h = 64;
  const int sel_y = sh - 168;
  const int center_x = sw / 2;
  out_buttons[n++] = {center_x - sel_w - 16, sel_y, sel_w, sel_h, "SELECT", HUD_CTRL_SELECT, false};
  out_buttons[n++] = {center_x + 16, sel_y, sel_w, sel_h, "START", HUD_CTRL_START, false};

  // Bottom utility toolbar: MODE (Quality/Turbo A2), PAL (DMG Palette), CLEAN, ROMS, EXIT
  const int util_count = 5;
  const int margin = 36;
  const int gap = 14;
  const int util_w = (sw - margin * 2 - gap * (util_count - 1)) / util_count;
  const int util_h = 68;
  const int util_y = sh - util_h - 20;

  out_buttons[n++] = {margin + 0 * (util_w + gap), util_y, util_w, util_h, "MODE", HUD_CTRL_MODE, false};
  out_buttons[n++] = {margin + 1 * (util_w + gap), util_y, util_w, util_h, "PALETTE", HUD_CTRL_PALETTE, false};
  out_buttons[n++] = {margin + 2 * (util_w + gap), util_y, util_w, util_h, "CLEAN", HUD_CTRL_CLEAN, false};
  out_buttons[n++] = {margin + 3 * (util_w + gap), util_y, util_w, util_h, "ROMS", HUD_CTRL_ROMS, false};
  out_buttons[n++] = {margin + 4 * (util_w + gap), util_y, util_w, util_h, "EXIT", HUD_CTRL_EXIT, false};

  return n;
}

void kobo_input_set_touch_layout(const kobo_display_info_t* display_info,
                                 const kobo_hud_button_t* buttons,
                                 size_t button_count) {
  if (display_info) g_input.disp_info = *display_info;
  g_input.hud_buttons.assign(buttons, buttons + button_count);
}

void kobo_input_feed_raw_event(uint16_t type, uint16_t code, int32_t value,
                               int device_kind, kobo_input_state_t* out_state) {
  g_input.last_device_kind = device_kind;
  if (type == kEvKey && code < g_input.held_keys.size() && value != 2) {
    g_input.held_keys.set(code, value != 0);
  }
  if (type == kEvKey) {
    if ((code == kKeyPower || code == kKeyHome || code == kKeyEsc) && value != 0) {
      g_input.pending_ui_action = INKBOY_ACTION_EXIT;
    } else if (code == kBtnTouch && value == 0) {
      // Single-touch or Type-A full release fallback
      if (!g_input.protocol_type_a_pending) {
        for (int s = 0; s < KOBO_MAX_TOUCH_SLOTS; ++s) {
          g_input.slots[s].active = false;
          g_input.slots[s].tracking_id = -1;
        }
      }
    } else {
      const uint8_t bit = mapKeycodeToPadBit(code);
      if (bit != 0) {
        if (value != 0) {
          g_input.physical_mask |= bit;
        } else {
          g_input.physical_mask &= static_cast<uint8_t>(~bit);
        }
      }
    }
  } else if (type == kEvAbs) {
    if (code == kAbsMtSlot) {
      if (value >= 0 && value < KOBO_MAX_TOUCH_SLOTS) {
        g_input.current_slot = value;
      }
    } else if (code == kAbsMtTrackingId) {
      kobo_touch_slot_t& slot = g_input.slots[g_input.current_slot];
      slot.tracking_id = value;
      slot.active = (value >= 0);
    } else if (code == kAbsMtPositionX || code == kAbsX) {
      kobo_touch_slot_t& slot = g_input.slots[g_input.current_slot];
      slot.raw_x = value;
      if (code == kAbsX) slot.active = true;
    } else if (code == kAbsMtPositionY || code == kAbsY) {
      kobo_touch_slot_t& slot = g_input.slots[g_input.current_slot];
      slot.raw_y = value;
      if (code == kAbsY) slot.active = true;
    } else if (code == kAbsHat0X) {
      g_input.physical_mask &= static_cast<uint8_t>(~(INKBOY_PAD_LEFT | INKBOY_PAD_RIGHT));
      if (value < 0) g_input.physical_mask |= INKBOY_PAD_LEFT;
      if (value > 0) g_input.physical_mask |= INKBOY_PAD_RIGHT;
    } else if (code == kAbsHat0Y) {
      g_input.physical_mask &= static_cast<uint8_t>(~(INKBOY_PAD_UP | INKBOY_PAD_DOWN));
      if (value < 0) g_input.physical_mask |= INKBOY_PAD_UP;
      if (value > 0) g_input.physical_mask |= INKBOY_PAD_DOWN;
    }
  } else if (type == kEvSyn) {
    if (code == kSynMtReport) {
      // Multitouch Type A protocol: current slot finger point is complete, advance cursor
      g_input.protocol_type_a_pending = true;
      g_input.slots[g_input.current_slot].active = true;
      if (g_input.current_slot + 1 < KOBO_MAX_TOUCH_SLOTS) {
        g_input.current_slot++;
      }
      g_input.type_a_slot_cursor = g_input.current_slot;
    } else if (code == kSynReport) {
      if (g_input.protocol_type_a_pending) {
        // If Type A emitted 0 points before SYN_REPORT, all fingers were lifted
        if (g_input.type_a_slot_cursor == 0) {
          for (int s = 0; s < KOBO_MAX_TOUCH_SLOTS; ++s) {
            g_input.slots[s].active = false;
          }
        } else {
          for (int s = g_input.type_a_slot_cursor; s < KOBO_MAX_TOUCH_SLOTS; ++s) {
            g_input.slots[s].active = false;
          }
        }
        g_input.current_slot = 0;
        g_input.type_a_slot_cursor = 0;
        g_input.protocol_type_a_pending = false;
      }
      evaluateTouchSlots();
      syncStateOutput(out_state);
    }
  }
}

void kobo_input_rescan_devices(void) {
#if defined(__linux__)
  if (g_input.epoll_fd < 0) return;
  for (int i = 0; i < 8; ++i) {
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
    const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;

    // Exclusively grab input device so Nickel does not receive ghost touches or button presses
    ::ioctl(fd, EVIOCGRAB, 1);

    struct epoll_event ev {};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(g_input.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
      g_input.device_fds.push_back(fd);
    } else {
      ::ioctl(fd, EVIOCGRAB, 0);
      ::close(fd);
    }
  }
#endif
}

bool kobo_input_wait_for_release(int timeout_ms) {
  int waited_ms = 0;
  for (;;) {
    kobo_input_state_t st {};
    kobo_input_poll(&st, 20);
    if (g_input.held_keys.none() && st.active_touch_count == 0) break;
    waited_ms += 20;
    if (waited_ms >= timeout_ms) return false;
  }
  // Drain anything still queued so nothing leaks to Nickel once the grab is released
  kobo_input_state_t drain {};
  kobo_input_poll(&drain, 0);
  return true;
}

void kobo_input_poll(kobo_input_state_t* out_state, int timeout_ms) {
#if defined(__linux__)
  if (g_input.epoll_fd >= 0) {
    struct epoll_event events[16];
    const int nfds = ::epoll_wait(g_input.epoll_fd, events, 16, timeout_ms);
    for (int i = 0; i < nfds; ++i) {
      const int fd = events[i].data.fd;
      struct input_event ie[32];
      ssize_t n = 0;
      while ((n = ::read(fd, ie, sizeof(ie))) > 0) {
        const size_t count = static_cast<size_t>(n) / sizeof(struct input_event);
        for (size_t k = 0; k < count; ++k) {
          kobo_input_feed_raw_event(ie[k].type, ie[k].code, ie[k].value, /*device_kind=*/1, nullptr);
        }
      }
    }
  }
#else
  (void)timeout_ms;
#endif
  syncStateOutput(out_state);
}

}  // extern "C"
