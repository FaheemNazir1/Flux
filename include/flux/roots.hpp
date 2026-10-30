#pragma once

#include "flux/database.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace flux {

// The root registry: the directories (or drives) a database indexes. Every document that was
// indexed through a registered root carries that root's id, so one root can be synced or removed
// without touching another's documents. Roots never overlap (one inside another), so a file
// belongs to at most one root.
//
// All functions need a schema-2 database; on a schema-1 database they fail with an explanatory error
// (opening it for writing upgrades it). Functions that modify the registry need a ReadWrite database.

struct RootInfo {
    std::int64_t id = 0;
    std::string path;            // canonical, true-case, forward slashes (see rootKey)
    std::int64_t addedAt = 0;    // unix seconds
    std::int64_t documents = 0;  // documents currently stored for this root
};

// All roots, ordered by path.
bool listRoots(Database& db, std::vector<RootInfo>& roots, std::string& error);

// Documents that belong to no root (made by plain indexing before roots existed).
bool countUnassignedDocuments(Database& db, std::int64_t& count, std::string& error);

// Finds the registered root a user-typed path refers to. Normally the path is resolved to its
// canonical form; if that is impossible (a disconnected drive, a deleted folder) it is matched as
// typed, ignoring ASCII letter case on Windows, so such a root can still be named. `found` is
// false (and `error` empty) when no registered root matches.
bool findRegisteredRoot(Database& db, const std::filesystem::path& userPath, RootInfo& root, bool& found,
                        std::string& error);

// The registered root containing the (canonical, stored-form) path `pathKey`, if any.
bool findRootContaining(Database& db, const std::string& pathKey, RootInfo& root, bool& found,
                        std::string& error);

struct AddRootResult {
    RootInfo root;
    bool alreadyRegistered = false;
    std::int64_t adoptedDocuments = 0;  // unassigned documents under the new root that now belong to it
};

// Registers `directory` (must exist; stored in canonical form). Registering a root that is already
// registered, however its path is spelled, changes nothing. A directory that contains or lies inside
// another registered root is refused. Unassigned documents under the new root are adopted by it.
// Runs in one transaction.
bool addRoot(Database& db, const std::filesystem::path& directory, AddRootResult& result, std::string& error);

struct RemoveRootResult {
    RootInfo root;
    std::int64_t removedDocuments = 0;
};

// Unregisters a root and deletes ITS documents, their postings, and terms nothing else uses, in one
// transaction. Other roots' documents, unassigned documents, and the files on disk are untouched.
// `directory` may name a root whose drive is currently not available.
bool removeRoot(Database& db, const std::filesystem::path& directory, RemoveRootResult& result, std::string& error);

}  // namespace flux
