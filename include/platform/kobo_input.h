#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "platform/kobo_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Joypad bits matching Peanut-GB / Walnut-CGB active-high mask before inversion */
#define INKBOY_PAD_A      0x01u
#define INKBOY_PAD_B      0x02u
#define INKBOY_PAD_SELECT 0x04u
#define INKBOY_PAD_START  0x08u
#define INKBOY_PAD_RIGHT  0x10u
#define INKBOY_PAD_LEFT   0x20u
#define INKBOY_PAD_UP     0x40u
#define INKBOY_PAD_DOWN   0x80u

/* HUD Control IDs */
typedef enum {
  HUD_CTRL_NONE = 0,
  HUD_CTRL_UP,
  HUD_CTRL_DOWN,
  HUD_CTRL_LEFT,
  HUD_CTRL_RIGHT,
  HUD_CTRL_A,
  HUD_CTRL_B,
  HUD_CTRL_SELECT,
  HUD_CTRL_START,
  HUD_CTRL_MODE,     /* Switch Quality (Color/4-Gray) <-> Turbo 1-bit A2 */
  HUD_CTRL_PALETTE,  /* Cycle DMG palette on Libra Colour */
  HUD_CTRL_ROTATE,   /* Switch Portrait <-> Landscape */
  HUD_CTRL_CLEAN,    /* Force full GC16/GCC16 anti-ghosting flash */
  HUD_CTRL_ROMS,     /* Return to ROM Selector */
  HUD_CTRL_EXIT      /* Save .sav and resume Nickel */
} inkboy_hud_ctrl_id_t;

typedef enum {
  INKBOY_ACTION_NONE = 0,
  INKBOY_ACTION_TOGGLE_MODE,
  INKBOY_ACTION_CYCLE_PALETTE,
  INKBOY_ACTION_ROTATE,
  INKBOY_ACTION_CYCLE_SCALE,
  INKBOY_ACTION_CLEAN_SCREEN,
  INKBOY_ACTION_ROM_MENU,
  INKBOY_ACTION_EXIT
} inkboy_ui_action_t;

typedef enum {
  LIBRA_BTN_MAP_A_B = 0,          /* Top page-turn = A, Bottom page-turn = B */
  LIBRA_BTN_MAP_B_A = 1,          /* Top page-turn = B, Bottom page-turn = A */
  LIBRA_BTN_MAP_START_SELECT = 2  /* Top = Start, Bottom = Select */
} libra_button_map_t;

#define KOBO_MAX_TOUCH_SLOTS 4

typedef struct {
  bool active;
  int tracking_id;
  int raw_x;
  int raw_y;
  int screen_x;
  int screen_y;
} kobo_touch_slot_t;

typedef struct {
  uint8_t joypad_mask;            /* Combined physical + Bluetooth + multitouch virtual pad */
  uint8_t physical_mask;          /* Physical Libra buttons + Bluetooth/USB gamepad */
  uint8_t touch_mask;             /* Multitouch virtual buttons on screen */
  inkboy_ui_action_t ui_action;   /* One-shot UI action triggered this frame */
  int active_touch_count;
  kobo_touch_slot_t slots[KOBO_MAX_TOUCH_SLOTS];
} kobo_input_state_t;

bool kobo_input_init(const kobo_display_info_t* display_info, libra_button_map_t btn_map);
void kobo_input_shutdown(void);

/* Build default ergonomic HUD buttons in the bottom (Portrait) or sidebars (Landscape) */
size_t kobo_input_build_default_hud(const kobo_display_info_t* display_info,
                                    kobo_hud_button_t* out_buttons,
                                    size_t max_buttons);

void kobo_input_set_touch_layout(const kobo_display_info_t* display_info,
                                 const kobo_hud_button_t* buttons,
                                 size_t button_count);

void kobo_input_poll(kobo_input_state_t* out_state, int timeout_ms);

/*
 * Before handing the input devices back to Nickel, wait until every key and finger is released
 * (up to timeout_ms) and drain pending events, so Nickel never receives an orphan key-up or
 * touch-up for a press it did not see. Returns false if the timeout expired.
 */
bool kobo_input_wait_for_release(int timeout_ms);
void kobo_input_rescan_devices(void);

/*
 * Feed a raw Linux input_event (type, code, value) directly into the state machine.
 * Used both by the real /dev/input/event* reader on Kobo and by unit tests to verify
 * Multitouch Type A / Type B, physical Libra buttons, and Bluetooth gamepads.
 */
void kobo_input_feed_raw_event(uint16_t type, uint16_t code, int32_t value,
                               int device_kind, kobo_input_state_t* out_state);

#ifdef __cplusplus
}
#endif
