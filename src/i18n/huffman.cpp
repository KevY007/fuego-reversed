// i18n/huffman.cpp
//
// See huffman.h for the full design/compatibility notes. Summary: this
// reproduces RakNet's default (languageId == 0) StringCompressor Huffman
// tree exactly -- same 256-entry frequency table, same tree-build
// algorithm (including tie-break order), same MSB-first bit packing --
// so that the codes assigned to each byte value match a real RakNet/
// SA:MP peer's. Everything in the anonymous namespace below is
// implementation detail with internal linkage only; encode()/decode()
// are the sole public surface.

#include "i18n/huffman.h"

#include <array>
#include <cstring>
#include <list>

namespace i18n::huffman {

namespace {

// RakNet's englishCharacterFrequencies[256], transcribed verbatim (index
// == byte value) from the canonical open-source release:
//   https://github.com/facebookarchive/RakNet/blob/master/Source/StringCompressor.cpp
// See huffman.h for why this exact table -- not merely "a" plausible
// English frequency table -- is required for wire compatibility.
constexpr std::array<uint32_t, 256> kEnglishCharacterFrequencies = {
    //   0      1      2      3      4      5      6      7      8      9     10     11     12     13     14     15
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,   722,     0,     0,     2,     0,     0,
    //  16     17     18     19     20     21     22     23     24     25     26     27     28     29     30     31
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    //  32(sp) 33(!)  34(")  35(#)  36($)  37(%)  38(&)  39(')  40(()  41())  42(*)  43(+)  44(,)  45(-)  46(.)  47(/)
    11084,    58,    63,     1,     0,    31,     0,   317,    64,    64,    44,     0,   695,    62,   980,   266,
    //  48(0)  49(1)  50(2)  51(3)  52(4)  53(5)  54(6)  55(7)  56(8)  57(9)  58(:)  59(;)  60(<)  61(=)  62(>)  63(?)
       69,    67,    56,     7,    73,     3,    14,     2,    69,     1,   167,     9,     1,     2,    25,    94,
    //  64(@)  65(A)  66(B)  67(C)  68(D)  69(E)  70(F)  71(G)  72(H)  73(I)  74(J)  75(K)  76(L)  77(M)  78(N)  79(O)
        0,   195,   139,    34,    96,    48,   103,    56,   125,   653,    21,     5,    23,    64,    85,    44,
    //  80(P)  81(Q)  82(R)  83(S)  84(T)  85(U)  86(V)  87(W)  88(X)  89(Y)  90(Z)  91([)  92(\)  93(])  94(^)  95(_)
       34,     7,    92,    76,   147,    12,    14,    57,    15,    39,    15,     1,     1,     1,     2,     3,
    //  96(`)  97(a)  98(b)  99(c) 100(d) 101(e) 102(f) 103(g) 104(h) 105(i) 106(j) 107(k) 108(l) 109(m) 110(n) 111(o)
        0,  3611,   845,  1077,  1884,  5870,   841,  1057,  2501,  3212,   164,   531,  2019,  1330,  3056,  4037,
    // 112(p)113(q) 114(r) 115(s) 116(t) 117(u) 118(v) 119(w) 120(x) 121(y) 122(z) 123({) 124(|) 125(}) 126(~) 127(DEL)
      848,    47,  2586,  2919,  4771,  1707,   535,  1106,   152,  1243,   100,     0,     2,     0,    10,     0,
    // 128..255: all 0 in RakNet's table (high-bit/extended byte values never appear in its English sample text)
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
};

// One node in the Huffman tree. Both leaves (256 of them, one per byte
// value) and internal (merge) nodes live in the same flat pool
// (Tree::nodes); children/parent are indices into that pool rather than
// raw pointers, so the pool can grow (std::vector) without invalidating
// existing links. -1 means "no such node" (root's parent; a leaf's
// children).
struct Node {
    uint32_t weight = 0;
    int left = -1;
    int right = -1;
    int parent = -1;
    uint8_t value = 0;

    bool is_leaf() const { return left == -1 && right == -1; }
};

struct Tree {
    std::vector<Node> nodes;                  // pool: 256 leaves + 255 internal nodes
    int root = -1;
    std::array<std::vector<bool>, 256> codes; // codes[byteValue] = root-to-leaf path, first-sent bit first
};

// Mirrors RakNet::HuffmanEncodingTree::InsertNodeIntoSortedList: insert
// `idx` into `list` (kept sorted ascending by weight) immediately before
// the first existing element whose weight is *not less than* idx's
// weight (so equal-weight nodes end up ordered most-recently-inserted
// first). This exact tie-break is part of RakNet's algorithm and must be
// reproduced for the resulting tree/codes to match RakNet's.
void insert_sorted(std::list<int>& list, const std::vector<Node>& nodes, int idx) {
    auto it = list.begin();
    while (it != list.end() && nodes[static_cast<size_t>(*it)].weight < nodes[static_cast<size_t>(idx)].weight) {
        ++it;
    }
    list.insert(it, idx);
}

// Mirrors RakNet::HuffmanEncodingTree::GenerateFromFrequencyTable exactly:
// 256 leaves (0-weight bumped to 1, "0 weights are illegal") are merged
// two-smallest-at-a-time until one root remains, then each leaf's code is
// derived by walking leaf -> root and reversing.
Tree build_tree() {
    Tree tree;
    tree.nodes.reserve(511); // 256 leaves + 255 internal nodes, exactly -- no reallocation during build
    std::array<int, 256> leafIndex{};
    std::list<int> worklist;

    for (int counter = 0; counter < 256; ++counter) {
        Node node;
        node.value = static_cast<uint8_t>(counter);
        node.weight = kEnglishCharacterFrequencies[static_cast<size_t>(counter)];
        if (node.weight == 0) {
            node.weight = 1; // "0 weights are illegal" -- RakNet comment, reproduced verbatim
        }
        tree.nodes.push_back(node);
        const int idx = static_cast<int>(tree.nodes.size()) - 1;
        leafIndex[static_cast<size_t>(counter)] = idx;
        insert_sorted(worklist, tree.nodes, idx);
    }

    for (;;) {
        const int lesser = worklist.front();
        worklist.pop_front();
        const int greater = worklist.front();
        worklist.pop_front();

        Node parent;
        parent.left = lesser;
        parent.right = greater;
        parent.weight = tree.nodes[static_cast<size_t>(lesser)].weight +
                         tree.nodes[static_cast<size_t>(greater)].weight;
        tree.nodes.push_back(parent);
        const int parentIdx = static_cast<int>(tree.nodes.size()) - 1;
        tree.nodes[static_cast<size_t>(lesser)].parent = parentIdx;
        tree.nodes[static_cast<size_t>(greater)].parent = parentIdx;

        if (worklist.empty()) {
            tree.root = parentIdx;
            break;
        }
        insert_sorted(worklist, tree.nodes, parentIdx);
    }

    // Walk each leaf up to the root to build its code. This produces the
    // path in leaf-to-root order, so it's reversed into root-to-leaf
    // order (the order bits are actually sent/read) before storing.
    for (int counter = 0; counter < 256; ++counter) {
        std::vector<bool> leafToRoot;
        int cur = leafIndex[static_cast<size_t>(counter)];
        do {
            const int parent = tree.nodes[static_cast<size_t>(cur)].parent;
            leafToRoot.push_back(tree.nodes[static_cast<size_t>(parent)].left != cur);
            cur = parent;
        } while (cur != tree.root);

        auto& code = tree.codes[static_cast<size_t>(counter)];
        code.assign(leafToRoot.rbegin(), leafToRoot.rend());
    }

    return tree;
}

// Built once, lazily, on first use (function-local statics are
// thread-safe initialized-once in C++11 and later). Never exposed
// outside this translation unit -- callers only see encode()/decode().
const Tree& get_tree() {
    static const Tree tree = build_tree();
    return tree;
}

// Appends bits MSB-first within each byte, matching RakNet::BitStream's
// convention (see huffman.h).
class BitWriter {
public:
    void put_bit(bool bit) {
        if ((bitCount_ & 7u) == 0u) {
            bytes_.push_back(0);
        }
        if (bit) {
            bytes_.back() |= static_cast<uint8_t>(0x80u >> (bitCount_ & 7u));
        }
        ++bitCount_;
    }

    void put_bits(const std::vector<bool>& code) {
        for (const bool bit : code) {
            put_bit(bit);
        }
    }

    std::vector<uint8_t> take_bytes() { return std::move(bytes_); }
    size_t bit_count() const { return bitCount_; }

private:
    std::vector<uint8_t> bytes_;
    size_t bitCount_ = 0;
};

// Reads one bit (MSB-first) from data[0..bitLength) at bit offset `pos`.
// Returns false (leaving `bit` unchanged) if `pos >= bitLength`; in that
// case data[pos >> 3] is never accessed, so this never reads out of the
// bounds implied by bitLength.
bool read_bit(const uint8_t* data, size_t bitLength, size_t pos, bool& bit) {
    if (pos >= bitLength) {
        return false;
    }
    const uint8_t byte = data[pos >> 3];
    bit = (byte & static_cast<uint8_t>(0x80u >> (pos & 7u))) != 0;
    return true;
}

// Mirrors RakNet::BitStream::ReadCompressed<unsigned short> (equivalently,
// this project's own BitStream::ReadCompressed(unsigned char*, size=16,
// unsignedData=true) in src/net/BitStream.cpp -- read there for the
// byte-array version this is a bit-position-based transcription of) bit
// for bit: a variable-length encoding where the upper byte of the 16-bit
// value is elided to a single `1` bit when it is zero, and otherwise a
// `0` bit is followed by both bytes written in full -- except if the
// upper byte elides to zero, in which case the lower byte gets one more
// halving: a `1` bit + 4 bits if its own upper nibble is zero (i.e. the
// whole value is <16), else a `0` bit + the full 8 bits.
//
// This is `unsigned short`, NOT `uint32_t`/`unsigned int`: verified
// empirically against the real captures in tests/stringcompressor_test.cpp
// (a uint32_t-width header decodes to nothing useful; a 16-bit header's
// derived stringBitLength is, in every capture, both a plausible size
// AND an exact multiple of 8 -- expected, since
// HuffmanEncodingTree::EncodeArray's own padding always pads the Huffman
// payload to a byte boundary before StringCompressor measures its bit
// length -- which a wrong header width would not produce by chance).
// SA:MP's RakNet fork apparently predates whatever RakNet revision widened
// this field to a full unsigned int.
//
// Advances `pos` past exactly what was read on success. Returns false
// (with `pos` left partway through) if `bitLength` is exhausted first.
bool read_compressed_uint16(const uint8_t* data, size_t bitLength, size_t& pos, uint16_t& value) {
    uint8_t bytes[2] = {0, 0}; // little-endian in-memory layout of `value` (see huffman.h note)
    int currentByte = 1;

    while (currentByte > 0) {
        bool matches = false;
        if (!read_bit(data, bitLength, pos, matches)) {
            return false;
        }
        ++pos;

        if (matches) {
            bytes[currentByte] = 0;
            --currentByte;
            continue;
        }

        // This byte (and everything below it) is sent in full, low byte first.
        const int bitsToRead = (currentByte + 1) * 8;
        for (int i = 0; i < bitsToRead; ++i) {
            bool bit = false;
            if (!read_bit(data, bitLength, pos, bit)) {
                return false;
            }
            ++pos;
            if (bit) {
                bytes[i / 8] |= static_cast<uint8_t>(0x80u >> (i % 8));
            }
        }
        std::memcpy(&value, bytes, sizeof(value));
        return true;
    }

    // currentByte == 0: every byte above it was zero. One more halving step.
    bool upperNibbleZero = false;
    if (!read_bit(data, bitLength, pos, upperNibbleZero)) {
        return false;
    }
    ++pos;

    if (upperNibbleZero) {
        uint8_t nibble = 0;
        for (int i = 0; i < 4; ++i) {
            bool bit = false;
            if (!read_bit(data, bitLength, pos, bit)) {
                return false;
            }
            ++pos;
            if (bit) {
                nibble |= static_cast<uint8_t>(0x80u >> i);
            }
        }
        bytes[0] = static_cast<uint8_t>(nibble >> 4); // the 4 bits were sent MSB-first, left-aligned
    } else {
        uint8_t byte0 = 0;
        for (int i = 0; i < 8; ++i) {
            bool bit = false;
            if (!read_bit(data, bitLength, pos, bit)) {
                return false;
            }
            ++pos;
            if (bit) {
                byte0 |= static_cast<uint8_t>(0x80u >> i);
            }
        }
        bytes[0] = byte0;
    }

    std::memcpy(&value, bytes, sizeof(value));
    return true;
}

// Inverse of read_compressed_uint16(): mirrors
// RakNet::BitStream::WriteCompressed<unsigned short> bit for bit,
// appending to `writer`. See read_compressed_uint16() for the scheme.
void write_compressed_uint16(BitWriter& writer, uint16_t value) {
    uint8_t bytes[2]; // little-endian in-memory layout of `value` (see huffman.h note)
    std::memcpy(bytes, &value, sizeof(value));

    int currentByte = 1;
    while (currentByte > 0) {
        if (bytes[currentByte] == 0) {
            writer.put_bit(true);
            --currentByte;
            continue;
        }

        writer.put_bit(false);
        const int bitsToWrite = (currentByte + 1) * 8;
        for (int i = 0; i < bitsToWrite; ++i) {
            const uint8_t b = bytes[i / 8];
            writer.put_bit((b & static_cast<uint8_t>(0x80u >> (i % 8))) != 0);
        }
        return;
    }

    // currentByte == 0: every byte above it was zero.
    if ((bytes[0] & 0xF0u) == 0x00u) {
        writer.put_bit(true);
        const uint8_t nibble = static_cast<uint8_t>(bytes[0] << 4); // move the meaningful low nibble to the high nibble
        for (int i = 0; i < 4; ++i) {
            writer.put_bit((nibble & static_cast<uint8_t>(0x80u >> i)) != 0);
        }
    } else {
        writer.put_bit(false);
        for (int i = 0; i < 8; ++i) {
            writer.put_bit((bytes[0] & static_cast<uint8_t>(0x80u >> i)) != 0);
        }
    }
}

} // namespace

std::vector<uint8_t> encode(const std::string& in, size_t& outBitLength) {
    const Tree& tree = get_tree();

    BitWriter writer;
    for (const char c : in) {
        const uint8_t byte = static_cast<uint8_t>(c);
        writer.put_bits(tree.codes[byte]);
    }

    outBitLength = writer.bit_count();
    return writer.take_bytes();
}

bool decode(const uint8_t* data, size_t bitLength, size_t charCount, std::string& out) {
    out.clear();
    if (charCount == 0) {
        return true;
    }
    if (data == nullptr) {
        return false;
    }

    const Tree& tree = get_tree();
    out.reserve(charCount);

    size_t pos = 0;
    int cur = tree.root;
    while (out.size() < charCount) {
        bool bit = false;
        if (!read_bit(data, bitLength, pos, bit)) {
            out.clear();
            return false; // ran out of bits before finishing charCount characters
        }
        ++pos;

        const Node& node = tree.nodes[static_cast<size_t>(cur)];
        cur = bit ? node.right : node.left;

        if (tree.nodes[static_cast<size_t>(cur)].is_leaf()) {
            out.push_back(static_cast<char>(tree.nodes[static_cast<size_t>(cur)].value));
            cur = tree.root;
        }
    }
    return true;
}

bool decode_string(const uint8_t* data, size_t numBytes, std::string& out) {
    out.clear();
    if (data == nullptr || numBytes == 0) {
        return false;
    }

    const size_t totalBits = numBytes * 8;
    size_t pos = 0;

    // Step 1: BitStream::ReadCompressed<unsigned short> -- the Huffman
    // payload's bit length, NOT a char/byte count (see huffman.h framing
    // write-up; this field is 16 bits wide, not 32 -- verified against
    // the real captures, see read_compressed_uint16()'s comment).
    uint16_t stringBitLength = 0;
    if (!read_compressed_uint16(data, totalBits, pos, stringBitLength)) {
        return false;
    }

    // Step 2: exactly `stringBitLength` more bits must be available.
    if (totalBits - pos < stringBitLength) {
        return false;
    }

    // Step 3: HuffmanEncodingTree::DecodeArray -- bit-length-driven, NOT
    // char-count-driven like decode() above: walk every one of the
    // `stringBitLength` bits through the tree and keep whatever leaves
    // complete along the way. The end-of-payload padding trick (see
    // huffman.h) guarantees a trailing partial code never completes one
    // extra phantom character, so no explicit stop-at-N-chars logic (and
    // no maxCharsToWrite cap) is needed here.
    const Tree& tree = get_tree();
    int cur = tree.root;
    for (uint32_t i = 0; i < stringBitLength; ++i) {
        bool bit = false;
        read_bit(data, totalBits, pos, bit); // always succeeds: pos < totalBits, checked above
        ++pos;

        const Node& node = tree.nodes[static_cast<size_t>(cur)];
        cur = bit ? node.right : node.left;

        if (tree.nodes[static_cast<size_t>(cur)].is_leaf()) {
            out.push_back(static_cast<char>(tree.nodes[static_cast<size_t>(cur)].value));
            cur = tree.root;
        }
    }
    return true;
}

std::vector<uint8_t> encode_string(const std::string& in) {
    const Tree& tree = get_tree();

    // Step 1 (HuffmanEncodingTree::EncodeArray): Huffman-code every
    // character into an intermediate bit buffer.
    BitWriter payload;
    for (const char c : in) {
        payload.put_bits(tree.codes[static_cast<uint8_t>(c)]);
    }

    // Step 2 (EncodeArray's end-of-stream padding trick): if not already
    // byte-aligned, pad with a prefix of the first (lowest byte value)
    // symbol whose code is longer than the padding needed, so the pad
    // itself can never complete a leaf. See huffman.h framing write-up.
    const size_t bitsBeforePad = payload.bit_count();
    if (bitsBeforePad % 8 != 0) {
        const size_t remainingBits = 8 - (bitsBeforePad % 8);
        for (int counter = 0; counter < 256; ++counter) {
            const std::vector<bool>& code = tree.codes[static_cast<size_t>(counter)];
            if (code.size() > remainingBits) {
                for (size_t i = 0; i < remainingBits; ++i) {
                    payload.put_bit(code[i]);
                }
                break;
            }
        }
    }

    // The wire format's length field is 16 bits wide (see
    // read_compressed_uint16()'s comment) -- i.e. this frame format tops
    // out at 65535 encoded bits (~8000+ characters of English text, once
    // Huffman compression is accounted for), matching real SA:MP, whose
    // dialog/label text is bounded well under that in practice.
    const uint16_t stringBitLength = static_cast<uint16_t>(payload.bit_count());
    const std::vector<uint8_t> payloadBytes = payload.take_bytes();

    // Step 3 (StringCompressor::EncodeString): compressed bit-length
    // prefix, then exactly `stringBitLength` raw payload bits.
    BitWriter out;
    write_compressed_uint16(out, stringBitLength);
    for (uint32_t i = 0; i < stringBitLength; ++i) {
        bool bit = false;
        read_bit(payloadBytes.data(), stringBitLength, i, bit); // always succeeds: i < stringBitLength
        out.put_bit(bit);
    }

    return out.take_bytes();
}

} // namespace i18n::huffman
