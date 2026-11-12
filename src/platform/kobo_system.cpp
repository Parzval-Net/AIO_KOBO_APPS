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
constexpr int kMaxNickelPids = 16;
pid_t g_nickel_pids[kMaxNickelPids] = {};
std::atomic<int> g_nickel_pid_count{0};
std::atomic<bool> g_nickel_suspended{false};

void resumeNickelAsyncSafe() {
  if (g_nickel_suspended.exchange(false, std::memory_order_acq_rel)) {
    const int count = g_nickel_pid_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
      const pid_t pid = g_nickel_pids[i];
      if (pid > 0) {
        ::kill(pid, SIGCONT);
      }
    }
  }
}

void findNickelPids() {
  int count = 0;
  DIR* proc = ::opendir("/proc");
  if (proc) {
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(proc)) != nullptr && count < kMaxNickelPids) {
      if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
      char* end = nullptr;
      const long pid = std::strtol(ent->d_name, &end, 10);
      if (pid <= 0 || !end || *end != '\0') continue;

      char comm_path[64];
      std::snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid);
      FILE* fp = std::fopen(comm_path, "r");
      if (!fp) continue;
      char comm[64] = {};
      const bool got = (std::fgets(comm, sizeof(comm), fp) != nullptr);
      std::fclose(fp);
      if (got) {
        const size_t len = std::strlen(comm);
        if (len > 0 && comm[len - 1] == '\n') comm[len - 1] = '\0';
        if (std::strcmp(comm, "nickel") == 0) {
          g_nickel_pids[count++] = static_cast<pid_t>(pid);
        }
      }
    }
    ::closedir(proc);
  }
  g_nickel_pid_count.store(count, std::memory_order_release);
}
#endif

void onPosixSignal(int sig) {
  (void)sig;
  g_quit_requested.store(true, std::memory_order_release);
}

void onFatalSignal(int sig) {
#if defined(__linux__)
  resumeNickelAsyncSafe();
#endif
  struct sigaction sa {};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, nullptr);
  ::raise(sig);
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

  struct sigaction fatal_sa {};
  fatal_sa.sa_handler = onFatalSignal;
  sigemptyset(&fatal_sa.sa_mask);
  fatal_sa.sa_flags = static_cast<int>(SA_RESETHAND);
  sigaction(SIGSEGV, &fatal_sa, nullptr);
  sigaction(SIGABRT, &fatal_sa, nullptr);
  sigaction(SIGBUS, &fatal_sa, nullptr);

  static bool atexit_registered = false;
  if (!atexit_registered) {
    std::atexit(kobo_system_shutdown);
    atexit_registered = true;
  }

#if defined(__linux__)
  if (cfg && cfg->suspend_nickel) {
    findNickelPids();
    const int count = g_nickel_pid_count.load(std::memory_order_acquire);
    bool stopped_any = false;
    for (int i = 0; i < count; ++i) {
      if (::kill(g_nickel_pids[i], SIGSTOP) == 0) {
        stopped_any = true;
      }
    }
    if (stopped_any) {
      g_nickel_suspended.store(true, std::memory_order_release);
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
  resumeNickelAsyncSafe();
  g_nickel_pid_count.store(0, std::memory_order_release);
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
      struct stat st {};
      if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        entries.push_back({full, label});
      }
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
