// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/lzx_decoder.h"

#include <algorithm>
#include <cstring>

namespace playden::runtime {
namespace {

constexpr unsigned kLiterals = 256;
constexpr unsigned kLengthSymbols = 249;
constexpr unsigned kAlignedSymbols = 8;
constexpr unsigned kPretreeSymbols = 20;
constexpr unsigned kMinMatch = 2;
constexpr unsigned kMaxCodeBits = 16;
constexpr std::size_t kFrameSize = 32768;

enum BlockType : unsigned { kVerbatim = 1, kAligned = 2, kUncompressed = 3 };

// Position slots: extra bits and base offset (the formatted offset before the 2 is taken off).
struct Slots {
  std::uint8_t extra[51];
  std::uint32_t base[51];
  Slots() {
    for (unsigned slot = 0, bits = 0; slot < 51; slot += 2) {
      extra[slot] = static_cast<std::uint8_t>(bits);
      if (slot + 1 < 51) extra[slot + 1] = static_cast<std::uint8_t>(bits);
      if (slot != 0 && bits < 17) ++bits;
    }
    base[0] = 0;
    for (unsigned slot = 0; slot + 1 < 51; ++slot) base[slot + 1] = base[slot] + (1u << extra[slot]);
  }
};
const Slots kSlots;

std::uint32_t Get32(const unsigned char* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | static_cast<std::uint32_t>(bytes[1]) << 8 |
         static_cast<std::uint32_t>(bytes[2]) << 16 | static_cast<std::uint32_t>(bytes[3]) << 24;
}

void Put32(unsigned char* bytes, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) bytes[index] = static_cast<unsigned char>(value >> (index * 8));
}

}  // namespace

// A canonical Huffman code as a full 16-bit lookup: entry = symbol << 8 | code length, 0 when no
// code starts with those bits (an empty code). A code must fill the table or be empty.
struct LzxDecoder::Tree {
  std::vector<std::uint32_t> table;
  Tree() : table(1u << kMaxCodeBits, 0) {}
  bool Build(const std::uint8_t* lengths, std::size_t count) {
    std::fill(table.begin(), table.end(), 0);
    std::uint32_t code = 0;
    std::size_t filled = 0;
    for (unsigned length = 1; length <= kMaxCodeBits; ++length) {
      for (std::size_t symbol = 0; symbol < count; ++symbol) {
        if (lengths[symbol] != length) continue;
        const std::uint32_t span = 1u << (kMaxCodeBits - length);
        const std::uint32_t first = code << (kMaxCodeBits - length);
        if (first + span > table.size()) return false;  // over-subscribed
        std::fill(table.begin() + first, table.begin() + first + span, static_cast<std::uint32_t>(symbol) << 8 | length);
        ++code;
        filled += span;
      }
      code <<= 1;
    }
    return filled == 0 || filled == table.size();  // incomplete otherwise
  }
};

LzxDecoder::LzxDecoder(unsigned window_bits, std::vector<std::string_view> blocks) : blocks_(std::move(blocks)) {
  for (const auto& block : blocks_) {
    starts_.push_back(input_size_);
    input_size_ += block.size();
  }
  static constexpr unsigned kSlotsForBits[] = {30, 32, 34, 36, 38, 42, 50};  // window bits 15 to 21
  if (window_bits < 15 || window_bits > 21) {
    error_ = "unsupported LZX window size";
    return;
  }
  window_size_ = std::size_t{1} << window_bits;
  main_symbols_ = kLiterals + kSlotsForBits[window_bits - 15] * 8;
  window_ = std::make_unique<std::uint8_t[]>(window_size_);
  main_lengths_.assign(main_symbols_, 0);
  length_lengths_.assign(kLengthSymbols, 0);
  aligned_lengths_.assign(kAlignedSymbols, 0);
  pretree_ = std::make_unique<Tree>();
  main_ = std::make_unique<Tree>();
  length_ = std::make_unique<Tree>();
  aligned_ = std::make_unique<Tree>();
}

LzxDecoder::~LzxDecoder() = default;

bool LzxDecoder::Failed(const char* why) {
  if (!error_) error_ = why;
  return false;
}

// The stream's byte at `at` (below input_size_).
unsigned char LzxDecoder::ByteAt(std::size_t at) {
  if (at < starts_[block_] || at >= starts_[block_] + blocks_[block_].size())
    block_ = static_cast<std::size_t>(std::upper_bound(starts_.begin(), starts_.end(), at) - starts_.begin()) - 1;
  while (blocks_[block_].empty()) ++block_;  // an empty block shares its start with the next one
  return static_cast<unsigned char>(blocks_[block_][at - starts_[block_]]);
}

// Copies the next `count` bytes of the stream, false when it ends first.
bool LzxDecoder::CopyInput(std::size_t count, unsigned char* out) {
  if (count > input_size_ - std::min(input_at_, input_size_)) return false;
  while (count > 0) {
    ByteAt(input_at_);  // selects the block
    const auto offset = input_at_ - starts_[block_];
    const auto take = std::min(count, blocks_[block_].size() - offset);
    std::memcpy(out, blocks_[block_].data() + offset, take);
    out += take;
    input_at_ += take;
    count -= take;
  }
  return true;
}

// Keeps at least `count` bits (at most 32) in the buffer, a 16-bit word at a time.
void LzxDecoder::Ensure(unsigned count) {
  while (bits_ < count) {
    std::uint32_t word = 0;
    if (input_at_ + 2 <= input_size_) {
      word = ByteAt(input_at_) | static_cast<std::uint32_t>(ByteAt(input_at_ + 1)) << 8;
      input_at_ += 2;
    } else {
      padding_ += 2;  // past the end: zeros a lookup may look at but no code may use (DecodeFrame)
    }
    bit_buffer_ |= static_cast<std::uint64_t>(word) << (48 - bits_);
    bits_ += 16;
  }
}

unsigned LzxDecoder::Bits(unsigned count) {
  if (count == 0) return 0;
  Ensure(count);
  const auto value = static_cast<unsigned>(bit_buffer_ >> (64 - count));
  bit_buffer_ <<= count;
  bits_ -= count;
  return value;
}

bool LzxDecoder::Symbol(const Tree& tree, unsigned* symbol) {
  Ensure(kMaxCodeBits);
  const auto entry = tree.table[static_cast<std::size_t>(bit_buffer_ >> (64 - kMaxCodeBits))];
  if (entry == 0) return Failed("damaged LZX data (no such code)");
  const unsigned length = entry & 0xff;
  bit_buffer_ <<= length;
  bits_ -= length;
  *symbol = entry >> 8;
  return true;
}

// Code lengths [first, last) as differences from the previous block's, through a pretree.
bool LzxDecoder::ReadLengths(Tree& pretree, std::vector<std::uint8_t>& lengths, std::size_t first, std::size_t last) {
  std::uint8_t pre[kPretreeSymbols];
  for (auto& length : pre) length = static_cast<std::uint8_t>(Bits(4));
  if (!pretree.Build(pre, kPretreeSymbols)) return Failed("damaged LZX data (pretree)");
  for (std::size_t at = first; at < last;) {
    unsigned code;
    if (!Symbol(pretree, &code)) return false;
    if (code == 17 || code == 18) {
      const std::size_t run = code == 17 ? Bits(4) + 4 : Bits(5) + 20;
      if (run > last - at) return Failed("damaged LZX data (lengths run past their tree)");
      std::fill(lengths.begin() + at, lengths.begin() + at + run, 0);
      at += run;
    } else if (code == 19) {
      const std::size_t run = Bits(1) + 4;
      if (run > last - at) return Failed("damaged LZX data (lengths run past their tree)");
      if (!Symbol(pretree, &code) || code > 16) return Failed("damaged LZX data (pretree)");
      const auto value = static_cast<std::uint8_t>((lengths[at] + 17 - code) % 17);
      std::fill(lengths.begin() + at, lengths.begin() + at + run, value);
      at += run;
    } else {
      lengths[at] = static_cast<std::uint8_t>((lengths[at] + 17 - code) % 17);
      ++at;
    }
  }
  return true;
}

bool LzxDecoder::ReadBlockHeader() {
  // An uncompressed block of odd length is followed by a byte that realigns the stream.
  if (block_type_ == kUncompressed && (block_length_ & 1)) {
    if (input_at_ >= input_size_) return Failed("damaged LZX data (input ends early)");
    ++input_at_;
  }
  block_type_ = Bits(3);
  const unsigned high = Bits(16), low = Bits(8);
  block_length_ = block_remaining_ = static_cast<std::size_t>(high) << 8 | low;
  switch (block_type_) {
    case kAligned:
      for (auto& length : aligned_lengths_) length = static_cast<std::uint8_t>(Bits(3));
      if (!aligned_->Build(aligned_lengths_.data(), kAlignedSymbols)) return Failed("damaged LZX data (aligned tree)");
      [[fallthrough]];
    case kVerbatim:
      if (!ReadLengths(*pretree_, main_lengths_, 0, kLiterals) ||
          !ReadLengths(*pretree_, main_lengths_, kLiterals, main_symbols_))
        return false;
      if (!main_->Build(main_lengths_.data(), main_symbols_)) return Failed("damaged LZX data (main tree)");
      if (!ReadLengths(*pretree_, length_lengths_, 0, kLengthSymbols)) return false;
      if (!length_->Build(length_lengths_.data(), kLengthSymbols)) return Failed("damaged LZX data (length tree)");
      return true;
    case kUncompressed: {
      // 1 to 16 bits of padding up to the next word, then the repeated offsets as bytes; words
      // already in the buffer go back to the input.
      if (bits_ == 0) Ensure(16);
      const unsigned drop = bits_ % 16 ? bits_ % 16 : 16;
      bit_buffer_ <<= drop;
      bits_ -= drop;
      input_at_ -= bits_ / 8;
      bit_buffer_ = 0;
      bits_ = 0;
      unsigned char stored[12];
      if (!CopyInput(sizeof(stored), stored)) return Failed("damaged LZX data (input ends early)");
      for (int index = 0; index < 3; ++index) repeated_[index] = Get32(stored + index * 4);
      return true;
    }
    default:
      return Failed("damaged LZX data (block type)");
  }
}

// Decodes up to *todo bytes of the current block into the window.
bool LzxDecoder::DecodeRun(std::size_t* todo) {
  std::size_t run = std::min(*todo, block_remaining_);
  if (block_type_ == kUncompressed) {
    if (!CopyInput(run, window_.get() + window_position_)) return Failed("damaged LZX data (input ends early)");
    window_position_ += run;
    decoded_ += run;
    block_remaining_ -= run;
    *todo -= run;
    return true;
  }
  std::size_t done = 0;
  while (done < run) {
    unsigned element;
    if (!Symbol(*main_, &element)) return false;
    if (element < kLiterals) {
      window_[window_position_++] = static_cast<std::uint8_t>(element);
      ++done;
      ++decoded_;
      continue;
    }
    element -= kLiterals;
    std::size_t length = element & 7;
    if (length == 7) {
      unsigned extra;
      if (!Symbol(*length_, &extra)) return false;
      length += extra;
    }
    length += kMinMatch;
    const unsigned slot = element >> 3;
    std::uint32_t offset;
    if (slot > 2) {
      const unsigned extra = kSlots.extra[slot];
      offset = kSlots.base[slot] - 2;
      if (block_type_ == kAligned && extra >= 3) {
        offset += Bits(extra - 3) << 3;
        unsigned aligned;
        if (!Symbol(*aligned_, &aligned)) return false;
        offset += aligned;
      } else {
        offset += Bits(extra);
      }
      repeated_[2] = repeated_[1];
      repeated_[1] = repeated_[0];
      repeated_[0] = offset;
    } else {
      offset = repeated_[slot];
      repeated_[slot] = repeated_[0];
      repeated_[0] = offset;
    }
    // A match stays inside its block, its frame and the window, and refers only to bytes decoded before.
    if (done + length > run) return Failed("damaged LZX data (match past its block or frame)");
    if (window_position_ + length > window_size_) return Failed("damaged LZX data (match past the window)");
    if (offset == 0 || offset > decoded_ || offset > window_size_) return Failed("damaged LZX data (match before the start)");
    std::size_t from = (window_position_ + window_size_ - offset) % window_size_;
    for (std::size_t index = 0; index < length; ++index) {
      window_[window_position_++] = window_[from];
      from = (from + 1) % window_size_;
    }
    done += length;
    decoded_ += length;
  }
  block_remaining_ -= done;
  *todo -= done;
  return true;
}

bool LzxDecoder::DecodeFrame(std::size_t size, unsigned char* out, std::string* reason) {
  const auto fail = [&]() {
    if (reason) *reason = error_ ? error_ : "damaged LZX data";
    return false;
  };
  if (error_) return fail();
  // Every frame but the last is a full one, so frames never straddle the window's end.
  if (size == 0 || size > kFrameSize || ended_) {
    Failed("damaged cabinet (LZX frame size)");
    return fail();
  }
  if (window_position_ == window_size_) window_position_ = 0;
  if (size > window_size_ - window_position_) {
    Failed("damaged cabinet (LZX frame size)");
    return fail();
  }
  ended_ = size < kFrameSize;
  const std::size_t frame_start = window_position_;
  if (!header_read_) {
    // The stream starts with the x86 translation flag and, when set, the file size it uses.
    std::uint32_t translation = 0;
    if (Bits(1)) {
      const unsigned high = Bits(16), low = Bits(16);
      translation = high << 16 | low;
    }
    translation_size_ = static_cast<std::int32_t>(translation);
    header_read_ = true;
  }
  for (std::size_t todo = size; todo > 0;) {
    if (block_remaining_ == 0 && !ReadBlockHeader()) return fail();
    const auto before = todo;
    if (!DecodeRun(&todo)) return fail();
    if (todo == before && block_remaining_ != 0) {
      Failed("damaged LZX data (no progress)");
      return fail();
    }
  }
  // Bits used, not just looked at, must all be real input.
  if ((input_at_ + padding_) * 8 - bits_ > input_size_ * 8) {
    Failed("damaged LZX data (input ends early)");
    return fail();
  }
  // Frames end on a 16-bit boundary of the input.
  if (bits_ % 16) {
    const unsigned drop = bits_ % 16;
    bit_buffer_ <<= drop;
    bits_ -= drop;
  }
  std::memcpy(out, window_.get() + frame_start, size);
  // Undo the encoder's E8 translation: absolute call targets back to relative ones, in the
  // first 1 GiB of output, for frames longer than 10 bytes.
  if (translation_size_ != 0 && size > 10 && output_ < (std::uint64_t{1} << 30)) {
    const auto position = static_cast<std::int64_t>(output_);
    for (std::size_t at = 0; at < size - 10;) {
      if (out[at] != 0xe8) {
        ++at;
        continue;
      }
      const auto current = position + static_cast<std::int64_t>(at);
      const auto absolute = static_cast<std::int32_t>(Get32(out + at + 1));
      if (absolute >= -current && absolute < translation_size_) {
        const std::int64_t relative = absolute >= 0 ? absolute - current : static_cast<std::int64_t>(absolute) + translation_size_;
        Put32(out + at + 1, static_cast<std::uint32_t>(relative));
      }
      at += 5;
    }
  }
  output_ += size;
  return true;
}

}  // namespace playden::runtime
