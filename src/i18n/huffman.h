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

} // namespace i18n::huffman
