// i18n/huffman.h
//
// RakNet-compatible static Huffman string codec ("StringCompressor").
//
// SA:MP (and RakNet in general) Huffman-compresses on-the-wire strings
// (dialog text, 3D text labels, chat, etc.) using a SINGLE, STATIC Huffman
// tree built once from a fixed 256-entry "English character frequency"
// table -- NOT an adaptive, per-message tree. Both sides of the wire must
// build the exact same tree from the exact same table, or nothing decodes
// correctly. This module reproduces that default (languageId == 0) tree.
//
// What this module reproduces, and how it was verified:
//
//   - RakNet's `englishCharacterFrequencies[256]` table, transcribed
//     verbatim (all 256 values checked one-by-one against line numbers)
//     from the canonical open-source release:
//       https://github.com/facebookarchive/RakNet/blob/master/Source/StringCompressor.cpp
//     (Oculus VR / RakNet, BSD-licensed). Not reproduced from memory.
//
//   - RakNet's `HuffmanEncodingTree::GenerateFromFrequencyTable()` tree
//     build algorithm exactly, from:
//       https://github.com/facebookarchive/RakNet/blob/master/Source/DS_HuffmanEncodingTree.cpp
//     including its tie-break order: all 256 leaves are inserted, in
//     order (byte value 0, then 1, then 2, ...), into a list kept sorted
//     ascending by weight, where a new node is placed immediately before
//     the first existing node whose weight is *not less than* its own
//     (so among equal-weight nodes, the most-recently-inserted one sorts
//     first). The two front (= smallest-weight) nodes are then repeatedly
//     popped and merged until one root remains. A frequency of 0 is
//     bumped to weight 1, exactly as RakNet does ("0 weights are
//     illegal"), so all 256 byte values are always encodable/decodable.
//     This exact tie-break order matters: a different (e.g. heap-based,
//     or differently-ordered) construction can yield a differently-
//     shaped-but-still-"valid" Huffman tree that assigns different bit
//     codes to the same bytes, and would NOT interoperate with a real
//     RakNet/SA:MP peer on the wire even though it "works" internally.
//
//   - RakNet's bit order: `RakNet::BitStream::Write1()` / `Write0()` /
//     `ReadBit()` pack bits MOST-SIGNIFICANT-BIT FIRST within each byte
//     (bit 7 down to bit 0, i.e. `byte |= 0x80 >> (bitIndexInByte)`).
//     This module's internal bit writer/reader use that same convention.
//     Verified directly against both upstream RakNet's BitStream.cpp and
//     this project's own src/net/BitStream.cpp (identical `0x80 >> n`
//     masking in both).
//
// What this module intentionally does NOT include (left to a future
// caller, since it is wire-protocol/framing, not the codec itself):
//
//   - RakNet::StringCompressor::EncodeString/DecodeString's framing: a
//     wide/narrow-string flag, a separately bit-length-prefixed
//     (BitStream::WriteCompressed) count, and a maxCharsToWrite / null-
//     termination convention. Only the core "encode N raw bytes -> a
//     Huffman bit stream" / "decode N characters <- a Huffman bit
//     stream" primitives are implemented here. A caller that knows the
//     wire framing (which is protocol-specific -- e.g. SA:MP's RPCs)
//     builds it on top of encode()/decode().
//
//     UPDATE: that framing is now implemented too -- see
//     decode_string()/encode_string() below. (There turned out to be no
//     wide/narrow-string flag for the char*/std::string overloads SA:MP
//     uses; see the framing write-up further down for what the real
//     framing is.) encode()/decode() above are unchanged and still the
//     right choice for anyone who already knows their own bit length and
//     character count.
//
//   - RakNet::HuffmanEncodingTree::EncodeArray's end-of-stream byte-
//     alignment padding trick (when the encoded bit count isn't a
//     multiple of 8, it pads the last partial byte with a *prefix* of
//     some other, longer valid code -- chosen so that prefix can never
//     itself walk to a leaf and thus can never decode as a phantom extra
//     character). That trick exists because RakNet's DecodeArray loop
//     walks every bit of a byte-aligned input buffer, including trailing
//     pad bits. This module's decode() instead stops as soon as it has
//     decoded `charCount` characters, so it never looks at trailing pad
//     bits and the trick is unnecessary. Any unused bits in the final
//     byte returned by encode() are simply left 0. (This does not affect
//     wire compatibility of the actual per-symbol codes, which are
//     bit-for-bit identical to RakNet's -- it only affects what junk, if
//     any, sits in the unused tail bits of the last byte.)
//
//   - Per-language custom trees. RakNet supports building a tree from
//     sample text for other "language IDs" (GenerateTreeFromStrings);
//     only the default English/languageId==0 tree is implemented, since
//     that is what SA:MP's default text channel uses.
//
// Status: encode()/decode() form a verified, deterministic, internally-
// consistent round trip (see tests/huffman_test.cpp) built from RakNet's
// real algorithm and real table. Bit-for-bit wire compatibility against
// an actual captured SA:MP packet has NOT yet been verified -- that
// cross-check is still TODO for whoever wires this into the SA:MP RPC
// path.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace i18n::huffman {

// Huffman-encodes the raw bytes of `in` (every byte value 0..255 is
// encodable -- this is not text-only) using RakNet's static English-
// frequency tree (built lazily, once, on first use).
//
// Returns the packed output bytes, bits packed MSB-first within each
// byte (matching RakNet::BitStream -- see header comment above).
// `outBitLength` is set to the number of MEANINGFUL bits in the returned
// buffer; the returned vector's size is exactly ceil(outBitLength / 8),
// and any trailing unused bits in the last byte are 0.
//
// Pure function of `in`: encoding the same string twice yields identical
// bytes and an identical outBitLength.
//
// `in == ""` returns an empty vector with outBitLength == 0.
std::vector<uint8_t> encode(const std::string& in, size_t& outBitLength);

// Decodes exactly `charCount` characters from the Huffman bit stream
// held in [data, data + ceil(bitLength/8)), which holds `bitLength`
// meaningful bits (as produced by encode(), or a wire-compatible RakNet
// encoder using the same default tree). On success, `out` is cleared and
// set to the decoded characters (out.size() == charCount) and true is
// returned.
//
// Returns false, and clears `out`, if the stream runs out of bits before
// `charCount` characters can be fully decoded (truncated/malformed
// input). This never reads data at or past byte index
// ceil(bitLength / 8), i.e. it never reads out of the bounds implied by
// `bitLength`.
//
// `charCount == 0` always succeeds with `out` cleared, regardless of
// `data`/`bitLength` (data may be nullptr in that case).
bool decode(const uint8_t* data, size_t bitLength, size_t charCount, std::string& out);

// ---------------------------------------------------------------------
// StringCompressor wire framing (decode_string / encode_string)
// ---------------------------------------------------------------------
//
// Everything above (encode()/decode()) is the bare Huffman codec only.
// This section adds the actual RakNet::StringCompressor::EncodeString /
// DecodeString wire framing that SA:MP uses for dialog `szInfo` and 3D
// text label strings, reproduced from the canonical sources (fetched and
// read directly for this change, not from memory):
//   https://github.com/facebookarchive/RakNet/blob/master/Source/StringCompressor.cpp
//   https://github.com/facebookarchive/RakNet/blob/master/Source/DS_HuffmanEncodingTree.cpp
//   https://github.com/facebookarchive/RakNet/blob/master/Source/BitStream.cpp
//
// The verified framing (languageId == 0, the SA:MP default/English tree)
// for EncodeString(input, maxCharsToWrite<=0 i.e. no truncation, output,
// languageId=0) is:
//
//   1. A `BitStream::WriteCompressed<unsigned short>(stringBitLength)` /
//      `ReadCompressed<unsigned short>` field FIRST -- NOT a plain
//      fixed-width write, and NOT the `uint32_t`/`unsigned int` width the
//      current facebookarchive/RakNet source (StringCompressor.cpp,
//      fetched for this change) shows: SA:MP's RakNet fork evidently
//      predates whichever RakNet revision widened this field to a full
//      unsigned int, and uses the 16-bit form instead. This was confirmed
//      empirically, not just by reading source: a 32-bit-header
//      interpretation of the real captures below decodes to nothing (the
//      first bit already implies an implausibly large length); the
//      16-bit-header interpretation makes every one of the 5 captures
//      decode to clean English AND makes the derived stringBitLength land
//      exactly on a multiple of 8 in every case (expected -- see point 3
//      -- and not something a wrong header width would produce by
//      chance). This project's src/net/BitStream.cpp already implements
//      the same variable-length byte-elision scheme generically (see its
//      private WriteCompressed/ReadCompressed(unsigned char*, size,
//      unsignedData)); huffman.cpp's read_compressed_uint16()/
//      write_compressed_uint16() mirror it, specialized to 16 bits,
//      against a raw bit position instead of a BitStream object. The
//      value this field carries is a BIT count (the Huffman-encoded
//      payload's length in bits) -- not a character count, not a byte
//      count. There is NO leading bool/wide-narrow flag before it: that
//      was an open question in the header comment above (see "does NOT
//      include" section) and turned out not to exist for the const
//      char*/std::string EncodeString/DecodeString overloads SA:MP's
//      ANSI dialog/label text uses.
//   2. Exactly `stringBitLength` raw bits of Huffman-coded payload,
//      MSB-first, immediately following -- NOT byte-aligned. The
//      compressed length field is very often not a multiple of 8 bits
//      (e.g. 8 bits for a short length, 12, 19, ...), so the payload
//      usually starts mid-byte.
//   3. Per HuffmanEncodingTree::EncodeArray, the payload's own last byte
//      -- if `stringBitLength` isn't itself a multiple of 8 -- is padded
//      with a *prefix* of some other byte value's full Huffman code
//      (scanning byte values 0..255 in order, the first one whose code
//      is longer than the number of padding bits needed), so that prefix
//      can never itself walk to a leaf. `stringBitLength` (the value
//      transmitted in step 1) is measured AFTER this padding is added,
//      i.e. it already includes the padding bits.
//
// Point 3 is why decoding must be driven by the transmitted BIT length
// (walk every one of those bits through the tree and keep however many
// leaves happen to complete), not by a predetermined character count the
// way decode() above works: the receiver doesn't know the character
// count in advance. HuffmanEncodingTree::DecodeArray(bitstream,
// stringBitLength, maxCharsToWrite, output) discovers it as a side
// effect of walking exactly `stringBitLength` bits -- the padding trick
// guarantees the trailing partial code never completes one more phantom
// trailing character.
//
// Reproduced with the same little-endian-native-byte-order assumption
// RakNet's own WriteCompressed/ReadCompressed make (its BitStream.cpp
// literally comments "// PCs" at the byte-indexing line) -- valid for
// this project's Windows x86/x64 target.

// Decodes a full RakNet StringCompressor frame -- as produced by a real
// SA:MP server for dialog `szInfo` or a 3D text label's text, i.e.
// exactly RakNet::StringCompressor::DecodeString's wire format for the
// default (languageId == 0) English tree -- from the `numBytes` bytes at
// `data`. `data` does not need to hold exactly the frame's length:
// trailing bytes past the frame (an outer packet's byte-alignment
// padding, or -- not the case for szInfo/label text, which run to the
// end of their RPC -- a following sibling field) are fine, since the
// frame is self-delimiting (its leading compressed-length field says
// exactly how many payload bits follow); this never reads at or past
// `data + numBytes`.
//
// On success, clears `out` and sets it to every character
// StringCompressor would have decoded. Unlike RakNet's DecodeString,
// which truncates to a caller-supplied maxCharsToWrite buffer size,
// there is no such cap here -- the full string is always returned, since
// `out` is a std::string, not a fixed-size buffer. Returns true (with
// `out` cleared to "") for an empty encoded string, too.
//
// Returns false, and clears `out`, if `data` is null, `numBytes == 0`,
// or the buffer is too short to hold the declared frame (the compressed
// length prefix itself doesn't fully fit in `numBytes` bytes, or fewer
// than `stringBitLength` bits follow it) -- malformed/truncated input,
// never a crash or an out-of-bounds read.
bool decode_string(const uint8_t* data, size_t numBytes, std::string& out);

// Inverse of decode_string(): encodes `in` (every byte value 0..255 is
// encodable, same as encode() -- this is not text-only) into a full
// RakNet StringCompressor frame, matching what a real RakNet/SA:MP
// peer's StringCompressor::EncodeString(in.c_str(), <no truncation>,
// output, languageId=0) would produce on this (little-endian) target,
// including the exact end-of-payload padding trick described above.
// decode_string() applied to this function's output always yields back
// `in` exactly.
//
// `in == ""` returns the single byte {0xC0}: a 6-bit compressed encoding
// of stringBitLength == 0 (one "upper byte matches zero" bit, one "upper
// nibble of last byte is zero" bit, then a zero nibble -- see
// huffman.cpp) followed by zero payload bits, left-aligned in the byte
// with its 2 trailing bits unused/zero (matching how encode() itself
// leaves unused trailing bits in its last byte -- see above).
std::vector<uint8_t> encode_string(const std::string& in);

} // namespace i18n::huffman
