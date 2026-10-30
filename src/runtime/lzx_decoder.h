// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Microsoft's LZX compression as cabinets use it ([MS-CAB], the LZX folder type, and the LZX
// format it refers to): one decoder per folder, reading the folder's data blocks as one stream,
// in place (a cabinet can be tens of MiB, and the console's heap is small).
// Each data block decodes to one frame of up to 32 KiB that depends on the frames before it (the
// window, the repeated offsets and the Huffman code lengths carry over), and the encoder's x86
// call translation (E8) is undone on each frame.
class LzxDecoder {
 public:
  // window_bits: 15 to 21, from the folder's compression type ((type >> 8) & 0x1f). blocks: the
  // folder's data blocks' bytes, in order; they must outlive the decoder.
  LzxDecoder(unsigned window_bits, std::vector<std::string_view> blocks);
  ~LzxDecoder();
  LzxDecoder(const LzxDecoder&) = delete;
  LzxDecoder& operator=(const LzxDecoder&) = delete;

  // Decodes the next frame into exactly `size` bytes (1 to 32768; only the folder's last frame
  // may be shorter). false with a reason for damaged data; the decoder can't continue after that.
  bool DecodeFrame(std::size_t size, unsigned char* out, std::string* reason);

 private:
  struct Tree;
  bool ReadLengths(Tree& pretree, std::vector<std::uint8_t>& lengths, std::size_t first, std::size_t last);
  bool ReadBlockHeader();
  bool DecodeRun(std::size_t* todo);
  bool Symbol(const Tree& tree, unsigned* symbol);
  unsigned char ByteAt(std::size_t at);
  bool CopyInput(std::size_t count, unsigned char* out);
  void Ensure(unsigned count);
  unsigned Bits(unsigned count);
  bool Failed(const char* why);

  std::size_t window_size_ = 0;
  unsigned main_symbols_ = 0;
  std::unique_ptr<std::uint8_t[]> window_;
  std::size_t window_position_ = 0;
  std::uint64_t output_ = 0;   // bytes of finished frames, for the E8 translation
  std::uint64_t decoded_ = 0;  // bytes in the window so far, which a match may refer back to
  std::uint32_t repeated_[3] = {1, 1, 1};
  bool header_read_ = false;
  bool ended_ = false;  // a short frame was decoded: it was the last
  std::int32_t translation_size_ = 0;
  unsigned block_type_ = 0;
  std::size_t block_length_ = 0, block_remaining_ = 0;
  // 16-bit little-endian words, read most significant bit first, across the blocks.
  std::vector<std::string_view> blocks_;
  std::vector<std::size_t> starts_;  // each block's offset in the stream
  std::size_t input_size_ = 0, block_ = 0;
  std::size_t input_at_ = 0, padding_ = 0;
  std::uint64_t bit_buffer_ = 0;
  unsigned bits_ = 0;
  const char* error_ = nullptr;
  std::vector<std::uint8_t> main_lengths_, length_lengths_, aligned_lengths_;
  std::unique_ptr<Tree> pretree_, main_, length_, aligned_;
};

}  // namespace playden::runtime
