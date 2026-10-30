// Benchmarks content extraction per document format on generated documents (repeatable: fixed sizes, no randomness).
// Writes only into a private temporary folder, which it removes again. Use a Release build:
//   extract_bench [--files N] [--threads T]
#include "flux/extract.hpp"
#include "flux/tokenizer.hpp"
#include "../tests/zip_writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// Deterministic filler: words from a small vocabulary.
std::string sentence(std::size_t seed) {
    static const char* words[] = {"search", "index", "document", "report", "analysis", "quarterly", "budget", "project", "meeting", "invoice",
                                  "customer", "schedule", "release", "network", "storage", "thread", "writer", "engine", "query", "result"};
    std::string s;
    for (int i = 0; i < 12; ++i) s += std::string(words[(seed * 7 + i * 13 + seed / 3) % 20]) + (i == 11 ? "." : " ");
    return s;
}

struct Format {
    std::string name;
    std::function<void(const fs::path&, std::size_t rows)> make;
};

std::string xmlEscape(const std::string& s) { return s; }  // the filler has no markup characters

std::vector<Format> formats() {
    std::vector<Format> list;
    list.push_back({"txt", [](const fs::path& p, std::size_t rows) {
                        std::ofstream out(p, std::ios::binary);
                        for (std::size_t i = 0; i < rows; ++i) out << sentence(i) << "\n";
                    }});
    list.push_back({"csv", [](const fs::path& p, std::size_t rows) {
                        std::ofstream out(p, std::ios::binary);
                        for (std::size_t i = 0; i < rows; ++i) out << i << "," << sentence(i) << "," << i * 3 << "\n";
                    }});
    list.push_back({"html", [](const fs::path& p, std::size_t rows) {
                        std::ofstream out(p, std::ios::binary);
                        out << "<html><head><style>p{color:red}</style></head><body>";
                        for (std::size_t i = 0; i < rows; ++i) out << "<p class=\"x\">" << sentence(i) << "</p>\n";
                        out << "</body></html>";
                    }});
    list.push_back({"rtf", [](const fs::path& p, std::size_t rows) {
                        std::ofstream out(p, std::ios::binary);
                        out << "{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}";
                        for (std::size_t i = 0; i < rows; ++i) out << sentence(i) << "\\par\n";
                        out << "}";
                    }});
    list.push_back({"docx", [](const fs::path& p, std::size_t rows) {
                        std::vector<std::string> paragraphs;
                        for (std::size_t i = 0; i < rows; ++i) paragraphs.push_back(xmlEscape(sentence(i)));
                        std::ofstream(p, std::ios::binary) << testzip::docx(paragraphs);
                    }});
    list.push_back({"xlsx", [](const fs::path& p, std::size_t rows) {
                        std::string sheet = "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>";
                        std::string strings = "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
                        for (std::size_t i = 0; i < rows; ++i) {
                            strings += "<si><t>" + sentence(i) + "</t></si>";
                            sheet += "<row r=\"" + std::to_string(i + 1) + "\"><c r=\"A" + std::to_string(i + 1) + "\" t=\"s\"><v>" + std::to_string(i) +
                                     "</v></c><c r=\"B" + std::to_string(i + 1) + "\"><v>" + std::to_string(i * 7) + "</v></c></row>";
                        }
                        sheet += "</sheetData></worksheet>";
                        strings += "</sst>";
                        std::ofstream(p, std::ios::binary) << testzip::build(
                            {{"[Content_Types].xml", "<Types/>"},
                             {"xl/workbook.xml", "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"Data\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
                             {"xl/_rels/workbook.xml.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Target=\"worksheets/sheet1.xml\"/></Relationships>"},
                             {"xl/sharedStrings.xml", strings},
                             {"xl/worksheets/sheet1.xml", sheet}});
                    }});
    list.push_back({"pptx", [](const fs::path& p, std::size_t rows) {
                        testzip::Parts parts = {{"[Content_Types].xml", "<Types/>"}};
                        const std::size_t slides = std::max<std::size_t>(1, rows / 10);
                        for (std::size_t s = 0; s < slides; ++s) {
                            std::string body;
                            for (std::size_t i = 0; i < 10; ++i) body += "<a:p><a:r><a:t>" + sentence(s * 10 + i) + "</a:t></a:r></a:p>";
                            parts.push_back({"ppt/slides/slide" + std::to_string(s + 1) + ".xml",
                                             "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree><p:sp><p:txBody>" + body +
                                                 "</p:txBody></p:sp></p:spTree></p:cSld></p:sld>"});
                        }
                        std::ofstream(p, std::ios::binary) << testzip::build(parts);
                    }});
    list.push_back({"pdf (uncompressed)", [](const fs::path& p, std::size_t rows) {
                        const std::size_t perPage = 40;
                        const std::size_t pages = std::max<std::size_t>(1, rows / perPage);
                        std::string out = "%PDF-1.4\n";
                        std::vector<std::size_t> offsets(5 + 2 * pages, 0);
                        auto object = [&](std::size_t n, const std::string& body) {
                            offsets[n] = out.size();
                            out += std::to_string(n) + " 0 obj\n" + body + "\nendobj\n";
                        };
                        object(1, "<< /Type /Catalog /Pages 2 0 R >>");
                        object(3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
                        std::string kids;
                        for (std::size_t pg = 0; pg < pages; ++pg) {
                            kids += std::to_string(4 + 2 * pg) + " 0 R ";
                            object(4 + 2 * pg, "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 3 0 R >> >> /Contents " + std::to_string(5 + 2 * pg) + " 0 R >>");
                            std::string text = "BT /F1 10 Tf 72 760 Td 12 TL\n";
                            for (std::size_t i = 0; i < perPage; ++i) text += "(" + sentence(pg * perPage + i) + ") Tj T*\n";
                            text += "ET";
                            object(5 + 2 * pg, "<< /Length " + std::to_string(text.size()) + " >>\nstream\n" + text + "\nendstream");
                        }
                        object(2, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " >>");
                        const std::size_t xref = out.size();
                        out += "xref\n0 " + std::to_string(offsets.size()) + "\n0000000000 65535 f \n";
                        for (std::size_t n = 1; n < offsets.size(); ++n) {
                            char line[32];
                            std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[n]);
                            out += line;
                        }
                        out += "trailer\n<< /Size " + std::to_string(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
                        std::ofstream(p, std::ios::binary) << out;
                    }});
    return list;
}

std::size_t peakWorkingSetMiB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return counters.PeakWorkingSetSize / (1024 * 1024);
#else
    return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t files = 24, threads = 4, rows = 20000;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--files")) files = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--threads")) threads = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--rows")) rows = static_cast<std::size_t>(std::atoi(argv[i + 1]));
    }
    const fs::path dir = fs::temp_directory_path() / "flux_extract_bench";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    std::cout << "Flux extraction benchmark: " << files << " files per format, ~" << rows << " lines each, " << threads
              << " threads for the parallel run\n"
              << "(generated documents; one-time warm-up first; times are wall clock; extraction + tokenization, no database)\n\n";
    std::printf("%-20s %9s %10s %10s %12s %12s %9s\n", "format", "file KiB", "text KiB", "1 thread", "MiB/s (file)", "files/s", "N threads");

    for (const Format& format : formats()) {
        std::vector<fs::path> paths;
        std::uintmax_t bytes = 0;
        for (std::size_t i = 0; i < files; ++i) {
            const fs::path p = dir / (format.name.substr(0, format.name.find(' ')) + std::to_string(i) + "." + format.name.substr(0, format.name.find(' ')));
            format.make(p, rows);
            bytes += fs::file_size(p);
            paths.push_back(p);
        }
        std::size_t textBytes = 0;
        auto work = [&](const fs::path& p) {
            const flux::ExtractResult r = flux::extractContent(p);
            std::unordered_map<std::string, std::uint32_t> counts;
            flux::tokenizeCounts(r.text, counts, 48);
            return r.text.size();
        };
        work(paths[0]);  // warm-up
        const auto t0 = Clock::now();
        for (const fs::path& p : paths) textBytes += work(p);
        const double single = std::chrono::duration<double>(Clock::now() - t0).count();

        const auto t1 = Clock::now();
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        for (std::size_t t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                for (std::size_t i = next++; i < paths.size(); i = next++) work(paths[i]);
            });
        }
        for (auto& t : pool) t.join();
        const double parallel = std::chrono::duration<double>(Clock::now() - t1).count();
        std::printf("%-20s %9llu %10llu %9.2fs %12.1f %12.1f %8.2fs\n", format.name.c_str(),
                    static_cast<unsigned long long>(bytes / files / 1024), static_cast<unsigned long long>(textBytes / files / 1024), single,
                    static_cast<double>(bytes) / 1048576.0 / single, static_cast<double>(files) / single, parallel);
        for (const fs::path& p : paths) fs::remove(p, ec);
    }
    std::printf("\npeak working set of the whole run: %zu MiB\n", peakWorkingSetMiB());
    fs::remove_all(dir, ec);
    return 0;
}
