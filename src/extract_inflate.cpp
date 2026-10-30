// DEFLATE decoder (RFC 1951) and ZIP central-directory reader, written for Flux so that reading
// Office, OpenDocument, EPUB and PDF files needs no third-party library.
#include "extract_internal.hpp"

#include <algorithm>
#include <cstring>

namespace flux::extract_detail {
namespace {

constexpr int kFastBits = 9;

struct Huffman {
    std::uint16_t count[16] = {};
    std::uint16_t symbol[288] = {};
    std::uint16_t fast[1 << kFastBits] = {};  // (length << 9) | symbol, 0 = not decodable with kFastBits bits
};

// Builds a canonical decoding table. Returns false for an over-subscribed set of lengths.
bool buildHuffman(Huffman& h, const std::uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof h.count);
    std::memset(h.fast, 0, sizeof h.fast);
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    if (h.count[0] == n) return true;  // no codes: legal for an unused distance tree
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    std::uint16_t offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i) {
        if (lengths[i] != 0) h.symbol[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
    // Fast table for the short codes.
    unsigned code = 0;
    int index = 0;
    for (int len = 1; len <= kFastBits; ++len) {
        for (int k = 0; k < h.count[len]; ++k, ++code, ++index) {
            unsigned reversed = 0;
            for (int b = 0; b < len; ++b) reversed |= ((code >> b) & 1u) << (len - 1 - b);
            for (unsigned fill = reversed; fill < (1u << kFastBits); fill += (1u << len)) {
                h.fast[fill] = static_cast<std::uint16_t>((len << 9) | h.symbol[index]);
            }
        }
        code <<= 1;
    }
    return true;
}

class BitReader {
public:
    BitReader(const unsigned char* data, std::size_t size) : data_(data), size_(size) {}
    void refill() {
        while (count_ <= 56 && pos_ < size_) {
            bits_ |= static_cast<std::uint64_t>(data_[pos_++]) << count_;
            count_ += 8;
        }
    }
    // Reads n (0..32) bits; false if the input is exhausted.
    bool take(int n, std::uint32_t& value) {
        if (n == 0) { value = 0; return true; }
        refill();
        if (count_ < n) return false;
        value = static_cast<std::uint32_t>(bits_ & ((1ull << n) - 1));
        bits_ >>= n;
        count_ -= n;
        return true;
    }
    // Decodes one symbol; returns -1 on error.
    int decode(const Huffman& h) {
        refill();
        const std::uint16_t entry = h.fast[bits_ & ((1u << kFastBits) - 1)];
        if (entry != 0) {
            const int len = entry >> 9;
            if (count_ < len) return -1;
            bits_ >>= len;
            count_ -= len;
            return entry & 0x1FF;
        }
        int code = 0, first = 0, index = 0;
        std::uint64_t bits = bits_;
        for (int len = 1; len < 16; ++len) {
            if (count_ < len) return -1;
            code |= static_cast<int>(bits & 1);
            bits >>= 1;
            const int cnt = h.count[len];
            if (code - cnt < first) {
                bits_ >>= len;
                count_ -= len;
                return h.symbol[index + (code - first)];
            }
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
    // Drops the bits up to the next byte boundary and returns the unread whole bytes to the input.
    void alignToByte() {
        const int drop = count_ & 7;
        bits_ >>= drop;
        count_ -= drop;
    }
    // Position in the input of the first byte not yet consumed (after alignToByte).
    std::size_t bytePosition() const { return pos_ - static_cast<std::size_t>(count_ / 8); }
    void seek(std::size_t position) { pos_ = position; bits_ = 0; count_ = 0; }
    std::size_t size() const { return size_; }
    const unsigned char* data() const { return data_; }

private:
    const unsigned char* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    std::uint64_t bits_ = 0;
    int count_ = 0;
};

const std::uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint16_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const std::uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

InflateStatus inflateBlock(BitReader& in, std::string& out, std::size_t base, std::size_t maxOut, const Huffman& lit,
                           const Huffman& dist) {
    for (;;) {
        const int symbol = in.decode(lit);
        if (symbol < 0) return InflateStatus::Corrupt;
        if (symbol < 256) {
            if (out.size() - base >= maxOut) return InflateStatus::TooLarge;
            out.push_back(static_cast<char>(symbol));
            continue;
        }
        if (symbol == 256) return InflateStatus::Ok;
        const int index = symbol - 257;
        if (index >= 29) return InflateStatus::Corrupt;
        std::uint32_t extra = 0;
        if (!in.take(kLengthExtra[index], extra)) return InflateStatus::Corrupt;
        const std::size_t length = kLengthBase[index] + extra;
        const int distSymbol = in.decode(dist);
        if (distSymbol < 0 || distSymbol >= 30) return InflateStatus::Corrupt;
        if (!in.take(kDistExtra[distSymbol], extra)) return InflateStatus::Corrupt;
        const std::size_t distance = kDistBase[distSymbol] + extra;
        if (distance > out.size() - base) return InflateStatus::Corrupt;
        if (out.size() - base + length > maxOut) return InflateStatus::TooLarge;
        std::size_t from = out.size() - distance;
        if (distance >= length) {
            out.append(out, from, length);
        } else {
            for (std::size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
        }
    }
}

}  // namespace

InflateStatus inflateRaw(const unsigned char* data, std::size_t size, std::string& out, std::size_t maxOut,
                         std::size_t* consumed) {
    BitReader in(data, size);
    const std::size_t base = out.size();
    static thread_local Huffman fixedLit, fixedDist;
    static thread_local bool fixedReady = false;
    if (!fixedReady) {
        std::uint8_t lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        buildHuffman(fixedLit, lengths, 288);
        std::uint8_t d[30];
        std::fill(d, d + 30, std::uint8_t{5});
        buildHuffman(fixedDist, d, 30);
        fixedReady = true;
    }
    for (;;) {
        std::uint32_t last = 0, type = 0;
        if (!in.take(1, last) || !in.take(2, type)) return InflateStatus::Corrupt;
        InflateStatus status = InflateStatus::Ok;
        if (type == 0) {
            in.alignToByte();
            const std::size_t at = in.bytePosition();
            if (at + 4 > size) return InflateStatus::Corrupt;
            const std::size_t len = data[at] | (data[at + 1] << 8);
            const std::size_t nlen = data[at + 2] | (data[at + 3] << 8);
            if ((len ^ 0xFFFF) != nlen || at + 4 + len > size) return InflateStatus::Corrupt;
            if (out.size() - base + len > maxOut) return InflateStatus::TooLarge;
            out.append(reinterpret_cast<const char*>(data + at + 4), len);
            in.seek(at + 4 + len);
        } else if (type == 1) {
            status = inflateBlock(in, out, base, maxOut, fixedLit, fixedDist);
        } else if (type == 2) {
            std::uint32_t nlen = 0, ndist = 0, ncode = 0;
            if (!in.take(5, nlen) || !in.take(5, ndist) || !in.take(4, ncode)) return InflateStatus::Corrupt;
            nlen += 257; ndist += 1; ncode += 4;
            if (nlen > 286 || ndist > 30) return InflateStatus::Corrupt;
            static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t lengths[320] = {};
            for (std::uint32_t i = 0; i < ncode; ++i) {
                std::uint32_t v = 0;
                if (!in.take(3, v)) return InflateStatus::Corrupt;
                lengths[order[i]] = static_cast<std::uint8_t>(v);
            }
            Huffman codeLengths;
            if (!buildHuffman(codeLengths, lengths, 19)) return InflateStatus::Corrupt;
            std::uint8_t all[320] = {};
            std::uint32_t index = 0;
            while (index < nlen + ndist) {
                const int sym = in.decode(codeLengths);
                if (sym < 0) return InflateStatus::Corrupt;
                if (sym < 16) { all[index++] = static_cast<std::uint8_t>(sym); continue; }
                std::uint32_t repeat = 0, extra = 0;
                std::uint8_t value = 0;
                if (sym == 16) {
                    if (index == 0 || !in.take(2, extra)) return InflateStatus::Corrupt;
                    value = all[index - 1];
                    repeat = 3 + extra;
                } else if (sym == 17) {
                    if (!in.take(3, extra)) return InflateStatus::Corrupt;
                    repeat = 3 + extra;
                } else {
                    if (!in.take(7, extra)) return InflateStatus::Corrupt;
                    repeat = 11 + extra;
                }
                if (index + repeat > nlen + ndist) return InflateStatus::Corrupt;
                while (repeat--) all[index++] = value;
            }
            if (all[256] == 0) return InflateStatus::Corrupt;
            Huffman lit, dist;
            if (!buildHuffman(lit, all, static_cast<int>(nlen)) || !buildHuffman(dist, all + nlen, static_cast<int>(ndist))) {
                return InflateStatus::Corrupt;
            }
            status = inflateBlock(in, out, base, maxOut, lit, dist);
        } else {
            return InflateStatus::Corrupt;
        }
        if (status != InflateStatus::Ok) return status;
        if (last != 0) break;
    }
    in.alignToByte();
    if (consumed != nullptr) *consumed = in.bytePosition();
    return InflateStatus::Ok;
}

InflateStatus inflateZlib(const unsigned char* data, std::size_t size, std::string& out, std::size_t maxOut) {
    if (size < 2) return InflateStatus::Corrupt;
    const unsigned cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20) != 0) {
        // Not a zlib header: some producers write raw deflate. Try that before giving up.
        std::string attempt;
        const InflateStatus raw = inflateRaw(data, size, attempt, maxOut);
        if (raw == InflateStatus::Ok) { out += attempt; return raw; }
        return InflateStatus::Corrupt;
    }
    return inflateRaw(data + 2, size - 2, out, maxOut);
}

// ------------------------------------------------------------------------------------------------ sources

FileSource::FileSource(const std::filesystem::path& path) : in_(path, std::ios::binary) {
    if (!in_) return;
    in_.seekg(0, std::ios::end);
    const std::streamoff end = in_.tellg();
    if (end < 0) return;
    size_ = static_cast<std::uint64_t>(end);
    in_.seekg(0);
    ok_ = static_cast<bool>(in_);
}

bool FileSource::read(std::uint64_t offset, void* buffer, std::size_t count) const {
    if (offset + count > size_) return false;
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(in_.gcount()) == count;
}

bool MemorySource::read(std::uint64_t offset, void* buffer, std::size_t count) const {
    if (offset + count > bytes_.size()) return false;
    std::memcpy(buffer, bytes_.data() + offset, count);
    return true;
}

bool readAll(const ByteSource& source, std::string& out, std::uint64_t maxBytes) {
    out.clear();
    if (source.size() > maxBytes) return false;
    out.resize(static_cast<std::size_t>(source.size()));
    return out.empty() || source.read(0, out.data(), out.size());
}

// --------------------------------------------------------------------------------------------------- zip

namespace {
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint64_t le64(const unsigned char* p) { return le32(p) | (static_cast<std::uint64_t>(le32(p + 4)) << 32); }
}  // namespace

bool ZipArchive::open(const ByteSource& source, std::string& error) {
    source_ = &source;
    entries_.clear();
    const std::uint64_t total = source.size();
    if (total < 22) { error = "too small to be a ZIP file"; return false; }

    // End of central directory record: within the last 64 KiB + 22 bytes.
    const std::size_t tail = static_cast<std::size_t>(std::min<std::uint64_t>(total, 65535 + 22));
    std::string buffer(tail, '\0');
    if (!source.read(total - tail, buffer.data(), tail)) { error = "cannot read ZIP"; return false; }
    std::size_t eocd = std::string::npos;
    for (std::size_t i = tail - 22 + 1; i-- > 0;) {
        if (buffer[i] == 'P' && buffer[i + 1] == 'K' && buffer[i + 2] == 5 && buffer[i + 3] == 6) { eocd = i; break; }
    }
    if (eocd == std::string::npos) { error = "no ZIP directory (damaged or not a ZIP file)"; return false; }
    const unsigned char* e = reinterpret_cast<const unsigned char*>(buffer.data()) + eocd;
    std::uint64_t entryCount = le16(e + 10);
    std::uint64_t dirSize = le32(e + 12);
    std::uint64_t dirOffset = le32(e + 16);

    if ((entryCount == 0xFFFF || dirSize == 0xFFFFFFFFu || dirOffset == 0xFFFFFFFFu) && eocd >= 20) {
        const unsigned char* loc = e - 20;  // ZIP64 end-of-central-directory locator
        if (le32(loc) == 0x07064b50) {
            const std::uint64_t off64 = le64(loc + 8);
            unsigned char rec[56];
            if (off64 + 56 <= total && source.read(off64, rec, 56) && le32(rec) == 0x06064b50) {
                entryCount = le64(rec + 32);
                dirSize = le64(rec + 40);
                dirOffset = le64(rec + 48);
            }
        }
    }
    if (dirOffset + dirSize > total || dirSize > 64ull * 1024 * 1024 || entryCount > 1000000) {
        error = "ZIP directory is damaged";
        return false;
    }
    std::string dir(static_cast<std::size_t>(dirSize), '\0');
    if (dirSize > 0 && !source.read(dirOffset, dir.data(), dir.size())) { error = "cannot read ZIP directory"; return false; }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(dir.data());
    std::size_t pos = 0;
    for (std::uint64_t i = 0; i < entryCount; ++i) {
        if (pos + 46 > dir.size() || le32(p + pos) != 0x02014b50) { error = "ZIP directory is damaged"; entries_.clear(); return false; }
        ZipEntry entry;
        entry.flags = le16(p + pos + 8);
        entry.method = le16(p + pos + 10);
        entry.compressedSize = le32(p + pos + 20);
        entry.size = le32(p + pos + 24);
        const std::size_t nameLen = le16(p + pos + 28), extraLen = le16(p + pos + 30), commentLen = le16(p + pos + 32);
        entry.localOffset = le32(p + pos + 42);
        if (pos + 46 + nameLen + extraLen + commentLen > dir.size()) { error = "ZIP directory is damaged"; entries_.clear(); return false; }
        entry.name.assign(dir, pos + 46, nameLen);
        // ZIP64 extra field replaces 0xFFFFFFFF values in the order size, compressed, offset.
        const unsigned char* extra = p + pos + 46 + nameLen;
        for (std::size_t at = 0; at + 4 <= extraLen;) {
            const std::uint16_t id = le16(extra + at), len = le16(extra + at + 2);
            if (at + 4 + len > extraLen) break;
            if (id == 1) {
                std::size_t q = at + 4;
                if (entry.size == 0xFFFFFFFFu && q + 8 <= at + 4 + len) { entry.size = le64(extra + q); q += 8; }
                if (entry.compressedSize == 0xFFFFFFFFu && q + 8 <= at + 4 + len) { entry.compressedSize = le64(extra + q); q += 8; }
                if (entry.localOffset == 0xFFFFFFFFu && q + 8 <= at + 4 + len) { entry.localOffset = le64(extra + q); }
            }
            at += 4 + len;
        }
        entries_.push_back(std::move(entry));
        pos += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipEntry* ZipArchive::find(std::string_view name) const {
    for (const ZipEntry& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

ZipRead ZipArchive::read(const ZipEntry& entry, std::string& out, std::size_t maxOut) const {
    out.clear();
    if ((entry.flags & 1) != 0) return ZipRead::Encrypted;
    if (entry.method != 0 && entry.method != 8) return ZipRead::Unsupported;
    if (entry.size > maxOut || entry.compressedSize > maxOut + (maxOut >> 1) + 1024) return ZipRead::TooLarge;
    unsigned char header[30];
    if (!source_->read(entry.localOffset, header, 30) || le32(header) != 0x04034b50) return ZipRead::Corrupt;
    const std::uint64_t dataOffset = entry.localOffset + 30 + le16(header + 26) + le16(header + 28);
    if (dataOffset + entry.compressedSize > source_->size()) return ZipRead::Corrupt;
    std::string compressed(static_cast<std::size_t>(entry.compressedSize), '\0');
    if (!compressed.empty() && !source_->read(dataOffset, compressed.data(), compressed.size())) return ZipRead::Corrupt;
    if (entry.method == 0) {
        out = std::move(compressed);
        return ZipRead::Ok;
    }
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(entry.size, maxOut)));
    const InflateStatus status = inflateRaw(reinterpret_cast<const unsigned char*>(compressed.data()), compressed.size(), out, maxOut);
    if (status == InflateStatus::TooLarge) { out.clear(); return ZipRead::TooLarge; }
    if (status != InflateStatus::Ok) { out.clear(); return ZipRead::Corrupt; }
    return ZipRead::Ok;
}

}  // namespace flux::extract_detail
