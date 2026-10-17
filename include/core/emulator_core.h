#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#define PEANUT_GB_IS_LITTLE_ENDIAN 1
#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define PEANUT_GB_HEADER_ONLY
#include "peanut_gb.h"
#undef PEANUT_GB_HEADER_ONLY
}

#include "core/walnut_cgb_port.h"

namespace inkboy {

constexpr int kGameBoyWidth = 160;
constexpr int kGameBoyHeight = 144;
constexpr size_t kGameBoyPixels = kGameBoyWidth * kGameBoyHeight;
constexpr uint64_t kEmulationFramePeriodUs = 16743ULL;  // ~59.7275 Hz native Game Boy cadence
constexpr uint8_t kMaxCatchUpFrames = 4;

enum class DmgColorPalette : uint8_t {
  Grayscale = 0,     // Classic Pocket / Carta grayscale
  PokeRedBlue = 1,   // SGB Red/Blue high-contrast palette for Kaleido 3
  OriginalGreen = 2, // Classic DMG pea-soup green
  GbcGoldCyan = 3    // GBC Gold/Silver warm palette
};

struct EmulatorFrame {
  uint16_t rgb565[kGameBoyPixels];
  uint8_t shade4[kGameBoyPixels];   // 0 (white) .. 3 (black)
  uint8_t bw_bayer[kGameBoyPixels]; // 0 (black) or 3 (white) via 2x2 Bayer
  uint64_t frame_number = 0;
  bool is_cgb_color = false;
};

class EmulatorSession {
 public:
  EmulatorSession();
  ~EmulatorSession();

  bool loadRomFile(const std::string& path, std::string* out_error = nullptr);
  bool loadRomMemory(const uint8_t* data, size_t size, const std::string& virtual_path,
                     std::string* out_error = nullptr);
  void shutdown();

  // Execute exactly one Game Boy frame (~16.743 ms of emulated time)
  void stepFrame();

  // Run time-based catch-up emulation at 59.7275 Hz
  uint32_t runUntil(uint64_t now_us);

  // Set active-high joypad mask (INKBOY_PAD_*)
  void setJoypadMask(uint8_t mask);
  uint8_t joypadMask() const { return joypad_mask_; }

  // Cycle DMG colorization palette (used when playing .gb ROMs on Kobo Libra Colour)
  void cycleDmgPalette();
  void setDmgPalette(DmgColorPalette pal) { dmg_palette_ = pal; }
  DmgColorPalette dmgPalette() const { return dmg_palette_; }

  // Lock-free triple-buffer consumer API for the display thread / loop
  const EmulatorFrame* acquireLatestFrame();
  void releaseAcquiredFrame();

  // Save & Load battery-backed Cartridge RAM (.sav) atomically
  bool loadSaveFile();
  bool saveCartRamAtomically();
  bool isSaveDirty() const;

  bool isLoaded() const { return loaded_; }
  bool isCgbRom() const { return use_walnut_cgb_; }
  const char* romTitle() const { return title_; }
  const std::string& romPath() const { return rom_path_; }
  const std::string& savePath() const { return save_path_; }
  size_t romSize() const { return rom_size_; }
  size_t saveSize() const { return cart_ram_size_; }
  uint64_t totalFramesRun() const { return total_frames_; }

 private:
  bool initCore(std::string* out_error);
  bool buildSavePaths();
  void publishCompletedFrame();
  void writeDmgLine(const uint8_t* pixels, uint_fast8_t line);
  void writeCgbLine(const uint16_t* rgb565_pixels, const uint8_t* shade4_pixels,
                    const uint8_t* bw_pixels, uint_fast8_t line);

  static uint8_t peanutRomRead(gb_s* gb, uint_fast32_t addr);
  static uint8_t peanutCartRamRead(gb_s* gb, uint_fast32_t addr);
  static void peanutCartRamWrite(gb_s* gb, uint_fast32_t addr, uint8_t val);
  static void peanutLcdDrawLine(gb_s* gb, const uint8_t* pixels, uint_fast8_t line);
  static void peanutFatalError(gb_s* gb, enum gb_error_e err, uint16_t addr);

  static void walnutColorLineCb(void* user, const uint16_t* rgb565_pixels,
                                const uint8_t* shade4_pixels, const uint8_t* bw_pixels,
                                uint_fast8_t line);
  static void walnutFatalCb(void* user, int error_code, uint16_t address);

  std::string rom_path_;
  std::string save_path_;
  std::string temp_save_path_;
  std::string backup_save_path_;

  bool loaded_ = false;
  bool use_walnut_cgb_ = false;
  gb_s* peanut_gb_ = nullptr;
  WalnutCgbContext* walnut_cgb_ = nullptr;

  uint8_t* rom_ = nullptr;
  size_t rom_size_ = 0;
  uint8_t* cart_ram_ = nullptr;
  size_t cart_ram_size_ = 0;
  bool save_dirty_ = false;

  uint8_t joypad_mask_ = 0;
  DmgColorPalette dmg_palette_ = DmgColorPalette::PokeRedBlue;
  char title_[32] = {};

  // Lock-free triple buffer (3 slots guarantee zero emulation stalls while EPDC refreshes).
  // Producer owns write_idx_, consumer owns read_idx_; the two only swap through middle_idx_,
  // so a slot can never be written while the display thread reads it.
  static constexpr uint8_t kFrameDirtyBit = 0x80;
  static constexpr uint8_t kFrameIndexMask = 0x03;
  EmulatorFrame frames_[3];
  uint8_t write_idx_ = 0;
  std::atomic<uint8_t> middle_idx_{1};
  uint8_t read_idx_ = 2;
  bool has_read_frame_ = false;

  uint64_t next_frame_us_ = 0;
  uint64_t total_frames_ = 0;
};

}  // namespace inkboy
