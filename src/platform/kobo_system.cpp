#include "platform/kobo_system.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_quit_requested{false};
#if defined(__linux__)
bool g_nickel_suspended = false;
#endif

void onPosixSignal(int sig) {
  (void)sig;
  g_quit_requested.store(true, std::memory_order_release);
}

bool hasGameBoyExtension(const char* name, std::string* out_clean_label) {
  const char* dot = std::strrchr(name, '.');
  if (!dot) return false;
  std::string ext(dot);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext != ".gb" && ext != ".gbc" && ext != ".cgb") return false;

  if (out_clean_label) {
    *out_clean_label = std::string(name);
  }
  return true;
}

}  // namespace

extern "C" {

bool kobo_system_init(const kobo_system_config_t* cfg) {
  g_quit_requested.store(false, std::memory_order_release);

  struct sigaction sa {};
  sa.sa_handler = onPosixSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);

#if defined(__linux__)
  if (cfg && cfg->suspend_nickel) {
    // Suspend Nickel cleanly via SIGSTOP so it stops polling touchscreen or updating the EPDC
    if (std::system("killall -STOP nickel >/dev/null 2>&1") == 0) {
      g_nickel_suspended = true;
    }
  }
#else
  (void)cfg;
#endif
  return true;
}

void kobo_system_shutdown(void) {
  ::sync();
#if defined(__linux__)
  if (g_nickel_suspended) {
    std::system("killall -CONT nickel >/dev/null 2>&1");
    g_nickel_suspended = false;
  }
#endif
}

bool kobo_system_should_quit(void) {
  return g_quit_requested.load(std::memory_order_acquire);
}

void kobo_system_request_quit(void) {
  g_quit_requested.store(true, std::memory_order_release);
}

uint64_t kobo_system_now_us(void) {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

void kobo_system_sleep_us(uint64_t us) {
  struct timespec req {};
  req.tv_sec = static_cast<time_t>(us / 1000000ULL);
  req.tv_nsec = static_cast<long>((us % 1000000ULL) * 1000ULL);
  nanosleep(&req, nullptr);
}

size_t kobo_system_scan_roms(const char* roms_dir, char rom_paths[][512],
                             char rom_names[][128], size_t max_roms) {
  if (!roms_dir || !rom_paths || !rom_names || max_roms == 0) return 0;

  DIR* dir = ::opendir(roms_dir);
  if (!dir) return 0;

  struct RomEntry {
    std::string full_path;
    std::string display_name;
  };
  std::vector<RomEntry> entries;

  struct dirent* ent = nullptr;
  while ((ent = ::readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    std::string label;
    if (hasGameBoyExtension(ent->d_name, &label)) {
      std::string full = std::string(roms_dir) + "/" + ent->d_name;
      entries.push_back({full, label});
    }
  }
  ::closedir(dir);

  std::sort(entries.begin(), entries.end(), [](const RomEntry& a, const RomEntry& b) {
    return a.display_name < b.display_name;
  });

  const size_t count = std::min(entries.size(), max_roms);
  for (size_t i = 0; i < count; ++i) {
    std::snprintf(rom_paths[i], 512, "%s", entries[i].full_path.c_str());
    std::snprintf(rom_names[i], 128, "%s", entries[i].display_name.c_str());
  }
  return count;
}

}  // extern "C"
