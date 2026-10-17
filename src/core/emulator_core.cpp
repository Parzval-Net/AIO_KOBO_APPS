#include "core/emulator_core.h"
#include "core/walnut_cgb_color.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace inkboy {

namespace {

constexpr size_t kRomHeaderSize = 0x150;
constexpr size_t kPeanutRomBankBytes = 0x4000;

size_t romBankCountFromCode(const uint8_t code) {
  if (code > 8) return 0;
  return static_cast<size_t>(2) << code;
}

bool isBatteryBackedCartType(const uint8_t type) {
  switch (type) {
    case 0x03:  // MBC1 + RAM + BATTERY
    case 0x06:  // MBC2 + BATTERY
    case 0x09:  // ROM + RAM + BATTERY
    case 0x0F:  // MBC3 + TIMER + BATTERY
    case 0x10:  // MBC3 + TIMER + RAM + BATTERY
    case 0x13:  // MBC3 + RAM + BATTERY
    case 0x1B:  // MBC5 + RAM + BATTERY
    case 0x1E:  // MBC7 + RAM + BATTERY
      return true;
    default:
      return false;
  }
}

bool hasGbcExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".gbc" || ext == ".cgb";
}

// 4-color RGB565 palettes for DMG games played on Kobo Libra Colour (Kaleido 3)
const uint16_t kDmgPalettes[4][4] = {
    // 0: Grayscale (White, Light Gray, Dark Gray, Black)
    {0xFFFFU, 0xAD55U, 0x52AAU, 0x0000U},
    // 1: PokeRedBlue (Warm Cream, Crimson Red, Royal Blue, Deep Navy/Black)
    {0xFFFBU, 0xF9A6U, 0x329BU, 0x0842U},
    // 2: OriginalGreen (DMG Olive Green)
    {0xDF72U, 0x8E0AU, 0x3446U, 0x0A22U},
    // 3: GbcGoldCyan (Warm Gold, Amber, Teal, Dark Slate)
    {0xFFDEU, 0xFD24U, 0x1C96U, 0x0863U},
};

// Ink-boy's exact 1-bit dither for DMG shades 0..3
inline uint8_t dmgShadeToBw(const uint8_t shade, const uint_fast8_t x, const uint_fast8_t y) {
  switch (shade & 0x03U) {
    case 3:
      return 0U;  // black
    case 2:
      return (((x ^ y) & 1U) == 0U || (x & 1U) == 0U) ? 0U : 3U;
    case 1:
      return ((x & 1U) == 0U && (y & 1U) == 0U) ? 0U : 3U;
    default:
      return 3U;  // white
  }
}

}  // namespace

EmulatorSession::EmulatorSession() {
  for (auto& f : frames_) {
    std::fill_n(f.rgb565, kGameBoyPixels, static_cast<uint16_t>(0xFFFFU));
    std::memset(f.shade4, 0, sizeof(f.shade4));
    std::fill_n(f.bw_bayer, kGameBoyPixels, static_cast<uint8_t>(3U));
  }
}

EmulatorSession::~EmulatorSession() { shutdown(); }

void EmulatorSession::shutdown() {
  loaded_ = false;
  if (walnut_cgb_) {
    walnut_cgb_destroy(walnut_cgb_);
    walnut_cgb_ = nullptr;
  }
  if (peanut_gb_) {
    std::free(peanut_gb_);
    peanut_gb_ = nullptr;
  }
  if (cart_ram_) {
    std::free(cart_ram_);
    cart_ram_ = nullptr;
  }
  if (rom_) {
    std::free(rom_);
    rom_ = nullptr;
  }
  rom_size_ = 0;
  cart_ram_size_ = 0;
  save_dirty_ = false;
  write_idx_ = 0;
  middle_idx_.store(1, std::memory_order_release);
  read_idx_ = 2;
  has_read_frame_ = false;
  next_frame_us_ = 0;
  total_frames_ = 0;
}

bool EmulatorSession::loadRomFile(const std::string& path, std::string* out_error) {
  shutdown();

  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    if (out_error) *out_error = "Unable to open Game Boy ROM: " + path;
    return false;
  }

  std::fseek(fp, 0, SEEK_END);
  const long file_len = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);

  if (file_len < static_cast<long>(kRomHeaderSize)) {
    std::fclose(fp);
    if (out_error) *out_error = "ROM size is invalid";
    return false;
  }

  std::vector<uint8_t> buf(static_cast<size_t>(file_len));
  if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
    std::fclose(fp);
    if (out_error) *out_error = "ROM read failed";
    return false;
  }
  std::fclose(fp);

  if (!loadRomMemory(buf.data(), buf.size(), path, out_error)) {
    return false;
  }
  loadSaveFile();
  return true;
}

bool EmulatorSession::loadRomMemory(const uint8_t* data, size_t size,
                                    const std::string& virtual_path,
                                    std::string* out_error) {
  shutdown();
  if (!data || size < kRomHeaderSize) {
    if (out_error) *out_error = "ROM buffer too small";
    return false;
  }

  const uint8_t rom_size_code = data[0x148];
  const size_t bank_count = romBankCountFromCode(rom_size_code);
  if (bank_count == 0) {
    if (out_error) *out_error = "Unsupported ROM bank count code";
    return false;
  }

  const size_t expected_size = bank_count * kPeanutRomBankBytes;
  if (size < expected_size) {
    if (out_error) *out_error = "ROM file is incomplete";
    return false;
  }

  if (data[0x149] > 5) {
    if (out_error) *out_error = "Unsupported cartridge RAM code";
    return false;
  }

  rom_ = static_cast<uint8_t*>(std::malloc(expected_size));
  if (!rom_) {
    if (out_error) *out_error = "Out of memory allocating ROM";
    return false;
  }
  std::memcpy(rom_, data, expected_size);
  rom_size_ = expected_size;
  rom_path_ = virtual_path;

  if (!buildSavePaths()) {
    if (out_error) *out_error = "Invalid save path length";
    shutdown();
    return false;
  }

  // Follow Ink-boy's routing: .gbc/.cgb or CGB-flagged header routes to Walnut-CGB
  const uint8_t cgb_flag = rom_[0x143];
  use_walnut_cgb_ = hasGbcExtension(virtual_path) || (cgb_flag == 0x80 || cgb_flag == 0xC0);

  if (!initCore(out_error)) {
    shutdown();
    return false;
  }

  loaded_ = true;
  return true;
}

bool EmulatorSession::buildSavePaths() {
  if (rom_path_.empty()) {
    save_path_ = "/tmp/inkboy_mem.sav";
  } else {
    save_path_ = rom_path_;
    const size_t slash = save_path_.find_last_of('/');
    const size_t dot = save_path_.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
      save_path_ += ".sav";
    } else {
      save_path_.replace(dot, std::string::npos, ".sav");
    }
  }
  temp_save_path_ = save_path_ + ".tmp";
  backup_save_path_ = save_path_ + ".bak";
  return save_path_.size() < 512;
}

bool EmulatorSession::initCore(std::string* out_error) {
  time_t raw_time = 0;
  std::time(&raw_time);
  struct tm time_info {};
  localtime_r(&raw_time, &time_info);

  char game_title[17] = {};

  if (use_walnut_cgb_) {
    int init_res = WALNUT_CGB_INIT_NO_ERROR;
    walnut_cgb_ = walnut_cgb_create(rom_, rom_size_, nullptr, &EmulatorSession::walnutFatalCb,
                                    this, &init_res);
    if (!walnut_cgb_ || init_res != WALNUT_CGB_INIT_NO_ERROR) {
      if (out_error) *out_error = "Walnut-CGB init failed (code " + std::to_string(init_res) + ")";
      return false;
    }
    walnut_cgb_set_color_line_callback(walnut_cgb_, &EmulatorSession::walnutColorLineCb);
    walnut_cgb_set_rtc(walnut_cgb_, &time_info);
    walnut_cgb_get_rom_name(walnut_cgb_, game_title);
    cart_ram_size_ = walnut_cgb_save_size(walnut_cgb_);
  } else {
    peanut_gb_ = static_cast<gb_s*>(std::calloc(1, sizeof(gb_s)));
    if (!peanut_gb_) {
      if (out_error) *out_error = "Out of memory for Peanut-GB";
      return false;
    }
    const enum gb_init_error_e res =
        gb_init(peanut_gb_, &EmulatorSession::peanutRomRead, &EmulatorSession::peanutCartRamRead,
                &EmulatorSession::peanutCartRamWrite, &EmulatorSession::peanutFatalError, this);
    if (res != GB_INIT_NO_ERROR) {
      if (out_error) *out_error = "Peanut-GB init failed";
      return false;
    }
    size_t save_sz = 0;
    if (gb_get_save_size_s(peanut_gb_, &save_sz) < 0) {
      if (out_error) *out_error = "Unknown cartridge RAM size";
      return false;
    }
    if (save_sz > 0) {
      cart_ram_ = static_cast<uint8_t*>(std::calloc(1, save_sz));
      if (!cart_ram_) {
        if (out_error) *out_error = "Out of memory for cartridge RAM";
        return false;
      }
      cart_ram_size_ = save_sz;
    }
    gb_init_lcd(peanut_gb_, &EmulatorSession::peanutLcdDrawLine);
    gb_set_rtc(peanut_gb_, &time_info);
    gb_get_rom_name(peanut_gb_, game_title);
  }

  std::strncpy(title_, game_title, sizeof(title_) - 1);
  if (title_[0] == '\0') {
    std::strncpy(title_, use_walnut_cgb_ ? "GAME BOY COLOR" : "GAME BOY", sizeof(title_) - 1);
  }
  setJoypadMask(0);
  return true;
}

void EmulatorSession::setJoypadMask(const uint8_t mask) {
  joypad_mask_ = mask;
  const uint8_t active_low = static_cast<uint8_t>(~mask);
  if (use_walnut_cgb_ && walnut_cgb_) {
    walnut_cgb_set_joypad(walnut_cgb_, active_low);
  } else if (peanut_gb_) {
    peanut_gb_->direct.joypad = active_low;
  }
}

void EmulatorSession::cycleDmgPalette() {
  const uint8_t next = (static_cast<uint8_t>(dmg_palette_) + 1U) & 0x03U;
  dmg_palette_ = static_cast<DmgColorPalette>(next);
}

void EmulatorSession::stepFrame() {
  if (!loaded_) return;
  if (use_walnut_cgb_ && walnut_cgb_) {
    walnut_cgb_run_frame(walnut_cgb_);
  } else if (peanut_gb_) {
    gb_run_frame(peanut_gb_);
  }
  ++total_frames_;
  frames_[write_idx_].frame_number = total_frames_;
  frames_[write_idx_].is_cgb_color = use_walnut_cgb_;
  publishCompletedFrame();
}

uint32_t EmulatorSession::runUntil(const uint64_t now_us) {
  if (!loaded_) return 0;
  if (next_frame_us_ == 0) next_frame_us_ = now_us;

  uint32_t ran = 0;
  while (now_us >= next_frame_us_ && ran < kMaxCatchUpFrames) {
    stepFrame();
    next_frame_us_ += kEmulationFramePeriodUs;
    ++ran;
  }
  if (ran == kMaxCatchUpFrames && now_us >= next_frame_us_) {
    next_frame_us_ = now_us + kEmulationFramePeriodUs;
  }
  return ran;
}

void EmulatorSession::publishCompletedFrame() {
  // Swap the completed slot into the middle (marked dirty) and continue on the slot it held
  const uint8_t prev_middle = middle_idx_.exchange(static_cast<uint8_t>(write_idx_ | kFrameDirtyBit),
                                                   std::memory_order_acq_rel);
  write_idx_ = static_cast<uint8_t>(prev_middle & kFrameIndexMask);
}

const EmulatorFrame* EmulatorSession::acquireLatestFrame() {
  // Take the newest frame only if one was published since the last acquire; otherwise keep
  // returning the frame already owned by the consumer (the display diff skips unchanged frames)
  if (middle_idx_.load(std::memory_order_acquire) & kFrameDirtyBit) {
    const uint8_t prev_middle = middle_idx_.exchange(read_idx_, std::memory_order_acq_rel);
    read_idx_ = static_cast<uint8_t>(prev_middle & kFrameIndexMask);
    has_read_frame_ = true;
  }
  return has_read_frame_ ? &frames_[read_idx_] : nullptr;
}

void EmulatorSession::releaseAcquiredFrame() {
  // No-op: the consumer keeps exclusive ownership of read_idx_ until its next acquire
}

bool EmulatorSession::isSaveDirty() const {
  if (use_walnut_cgb_ && walnut_cgb_) {
    return walnut_cgb_is_ram_dirty(walnut_cgb_) != 0;
  }
  return save_dirty_;
}

bool EmulatorSession::loadSaveFile() {
  if (!loaded_ || cart_ram_size_ == 0 || !isBatteryBackedCartType(rom_[0x147])) return true;

  uint8_t* dst = use_walnut_cgb_ ? walnut_cgb_save_ram(walnut_cgb_) : cart_ram_;
  if (!dst) return false;
  std::memset(dst, 0, cart_ram_size_);

  FILE* fp = std::fopen(save_path_.c_str(), "rb");
  if (!fp) return true;  // No existing save yet
  const size_t got = std::fread(dst, 1, cart_ram_size_, fp);
  std::fclose(fp);
  (void)got;
  if (use_walnut_cgb_) walnut_cgb_clear_ram_dirty(walnut_cgb_);
  save_dirty_ = false;
  return true;
}

bool EmulatorSession::saveCartRamAtomically() {
  if (!loaded_ || !isSaveDirty() || cart_ram_size_ == 0 || !isBatteryBackedCartType(rom_[0x147])) {
    return true;
  }

  const uint8_t* src = use_walnut_cgb_ ? walnut_cgb_save_ram(walnut_cgb_) : cart_ram_;
  if (!src) return false;

  const int fd = ::open(temp_save_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;

  size_t total_written = 0;
  while (total_written < cart_ram_size_) {
    const ssize_t w = ::write(fd, src + total_written, cart_ram_size_ - total_written);
    if (w <= 0) {
      ::close(fd);
      ::unlink(temp_save_path_.c_str());
      return false;
    }
    total_written += static_cast<size_t>(w);
  }

  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(temp_save_path_.c_str());
    return false;
  }
  ::close(fd);

  struct stat st {};
  const bool had_existing = (::stat(save_path_.c_str(), &st) == 0);
  if (had_existing) {
    ::unlink(backup_save_path_.c_str());
    if (::rename(save_path_.c_str(), backup_save_path_.c_str()) != 0) {
      ::unlink(temp_save_path_.c_str());
      return false;
    }
  }

  if (::rename(temp_save_path_.c_str(), save_path_.c_str()) != 0) {
    if (had_existing) ::rename(backup_save_path_.c_str(), save_path_.c_str());
    ::unlink(temp_save_path_.c_str());
    return false;
  }

  if (had_existing) ::unlink(backup_save_path_.c_str());
  if (use_walnut_cgb_) walnut_cgb_clear_ram_dirty(walnut_cgb_);
  save_dirty_ = false;
  return true;
}

void EmulatorSession::writeDmgLine(const uint8_t* pixels, const uint_fast8_t line) {
  if (!pixels || line >= kGameBoyHeight) return;
  EmulatorFrame& dst = frames_[write_idx_];
  const size_t row_offset = static_cast<size_t>(line) * kGameBoyWidth;
  const uint16_t* pal = kDmgPalettes[static_cast<uint8_t>(dmg_palette_) & 0x03U];

  for (uint_fast8_t x = 0; x < kGameBoyWidth; ++x) {
    const uint8_t shade = static_cast<uint8_t>(pixels[x] & 0x03U);
    dst.shade4[row_offset + x] = shade;
    dst.rgb565[row_offset + x] = pal[shade];
    dst.bw_bayer[row_offset + x] = dmgShadeToBw(shade, x, line);
  }
}

void EmulatorSession::writeCgbLine(const uint16_t* rgb565_pixels, const uint8_t* shade4_pixels,
                                   const uint8_t* bw_pixels, const uint_fast8_t line) {
  if (line >= kGameBoyHeight) return;
  EmulatorFrame& dst = frames_[write_idx_];
  const size_t row_offset = static_cast<size_t>(line) * kGameBoyWidth;
  std::memcpy(dst.rgb565 + row_offset, rgb565_pixels, kGameBoyWidth * sizeof(uint16_t));
  std::memcpy(dst.shade4 + row_offset, shade4_pixels, kGameBoyWidth);
  std::memcpy(dst.bw_bayer + row_offset, bw_pixels, kGameBoyWidth);
}

uint8_t EmulatorSession::peanutRomRead(gb_s* state, const uint_fast32_t addr) {
  auto* self = state ? static_cast<EmulatorSession*>(state->direct.priv) : nullptr;
  if (!self || !self->rom_ || addr >= self->rom_size_) return 0xFF;
  return self->rom_[addr];
}

uint8_t EmulatorSession::peanutCartRamRead(gb_s* state, const uint_fast32_t addr) {
  auto* self = state ? static_cast<EmulatorSession*>(state->direct.priv) : nullptr;
  if (!self || !self->cart_ram_ || addr >= self->cart_ram_size_) return 0xFF;
  return self->cart_ram_[addr];
}

void EmulatorSession::peanutCartRamWrite(gb_s* state, const uint_fast32_t addr, const uint8_t val) {
  auto* self = state ? static_cast<EmulatorSession*>(state->direct.priv) : nullptr;
  if (!self || !self->cart_ram_ || addr >= self->cart_ram_size_) return;
  if (self->cart_ram_[addr] != val) self->save_dirty_ = true;
  self->cart_ram_[addr] = val;
}

void EmulatorSession::peanutLcdDrawLine(gb_s* state, const uint8_t* pixels, const uint_fast8_t line) {
  auto* self = state ? static_cast<EmulatorSession*>(state->direct.priv) : nullptr;
  if (self) self->writeDmgLine(pixels, line);
}

void EmulatorSession::peanutFatalError(gb_s* state, enum gb_error_e err, const uint16_t addr) {
  auto* self = state ? static_cast<EmulatorSession*>(state->direct.priv) : nullptr;
  std::fprintf(stderr, "[Ink-boy] Peanut-GB fatal error=%d addr=0x%04X\n", static_cast<int>(err), addr);
  if (self) self->saveCartRamAtomically();
}

void EmulatorSession::walnutColorLineCb(void* user, const uint16_t* rgb565_pixels,
                                        const uint8_t* shade4_pixels, const uint8_t* bw_pixels,
                                        const uint_fast8_t line) {
  auto* self = static_cast<EmulatorSession*>(user);
  if (self) self->writeCgbLine(rgb565_pixels, shade4_pixels, bw_pixels, line);
}

void EmulatorSession::walnutFatalCb(void* user, const int error_code, const uint16_t address) {
  auto* self = static_cast<EmulatorSession*>(user);
  std::fprintf(stderr, "[Ink-boy] Walnut-CGB fatal error=%d addr=0x%04X\n", error_code, address);
  if (self) self->saveCartRamAtomically();
}

}  // namespace inkboy
