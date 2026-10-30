#pragma once
// A minimal ZIP writer for tests (entries are stored, not compressed): lets a test build a Word / PowerPoint /
// Excel / OpenDocument file of any size with exactly the words it wants, without any tool outside the build.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace testzip {

inline std::uint32_t crc32(const std::string& data) {
    static const std::vector<std::uint32_t> table = [] {
        std::vector<std::uint32_t> t(256);
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : data) crc = table[(crc ^ static_cast<unsigned char>(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline void put16(std::string& out, std::uint32_t v) { out.push_back(static_cast<char>(v & 0xFF)); out.push_back(static_cast<char>((v >> 8) & 0xFF)); }
inline void put32(std::string& out, std::uint32_t v) { put16(out, v & 0xFFFF); put16(out, v >> 16); }

using Parts = std::vector<std::pair<std::string, std::string>>;

inline std::string build(const Parts& parts) {
    std::string out, directory;
    for (const auto& part : parts) {
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32(part.second), size = static_cast<std::uint32_t>(part.second.size());
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0x21);
        put32(out, crc); put32(out, size); put32(out, size); put16(out, static_cast<std::uint32_t>(part.first.size())); put16(out, 0);
        out += part.first; out += part.second;
        put32(directory, 0x02014b50); put16(directory, 20); put16(directory, 20); put16(directory, 0); put16(directory, 0);
        put16(directory, 0); put16(directory, 0x21); put32(directory, crc); put32(directory, size); put32(directory, size);
        put16(directory, static_cast<std::uint32_t>(part.first.size())); put16(directory, 0); put16(directory, 0); put16(directory, 0);
        put16(directory, 0); put32(directory, 0); put32(directory, offset);
        directory += part.first;
    }
    const std::uint32_t directoryOffset = static_cast<std::uint32_t>(out.size());
    out += directory;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0);
    put16(out, static_cast<std::uint32_t>(parts.size())); put16(out, static_cast<std::uint32_t>(parts.size()));
    put32(out, static_cast<std::uint32_t>(directory.size())); put32(out, directoryOffset); put16(out, 0);
    return out;
}

inline void write(const std::filesystem::path& path, const Parts& parts) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << build(parts);
}

// A .docx whose body holds the given paragraphs.
inline std::string docx(const std::vector<std::string>& paragraphs) {
    std::string body;
    for (const std::string& p : paragraphs) body += "<w:p><w:r><w:t>" + p + "</w:t></w:r></w:p>";
    return build({{"[Content_Types].xml", "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>"},
                  {"word/document.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>" + body + "</w:body></w:document>"}});
}

}  // namespace testzip
