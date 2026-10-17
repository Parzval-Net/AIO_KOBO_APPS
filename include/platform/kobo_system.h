#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool suspend_nickel;          /* Send SIGSTOP to nickel on enter, SIGCONT on exit */
  const char* base_dir;         /* Default: "/mnt/onboard/.adds/inkboy" */
  const char* roms_dir;         /* Default: "/mnt/onboard/.adds/inkboy/roms" */
} kobo_system_config_t;

bool kobo_system_init(const kobo_system_config_t* cfg);
void kobo_system_shutdown(void);

bool kobo_system_should_quit(void);
void kobo_system_request_quit(void);

uint64_t kobo_system_now_us(void);
void kobo_system_sleep_us(uint64_t us);

/* List all .gb and .gbc ROM files in roms_dir (sorted alphabetically) */
size_t kobo_system_scan_roms(const char* roms_dir, char rom_paths[][512],
                             char rom_names[][128], size_t max_roms);

#ifdef __cplusplus
}
#endif
