#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flux {

enum class DriveType { Fixed, Removable, Network, Optical, RamDisk, Unknown };

const char* driveTypeName(DriveType type);  // "Fixed disk", "Removable drive", "Network drive", ...

// One storage location of the computer as Windows reports it right now.
struct DriveInfo {
    std::string root;        // "C:/" (the stored form of a root, see rootKey)
    char letter = 0;         // 'C'
    std::string label;       // volume name, UTF-8; may be empty
    DriveType type = DriveType::Unknown;
    std::string filesystem;  // "NTFS", "exFAT", ...; empty if unknown
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
    bool ready = false;      // the drive answered: a disc, card or network share is present and reachable
    bool isSystem = false;   // holds the Windows folder
    std::string note;        // why it is not ready ("not responding", "no medium"), else empty
};

// Lists the drives present now, ordered by letter. Reading a drive never writes to it and never needs
// administrator rights. A network share or removable drive that does not answer within a few seconds is
// listed as not ready instead of blocking. Empty on systems without drive letters.
std::vector<DriveInfo> discoverDrives();

}  // namespace flux
