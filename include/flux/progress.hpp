#pragma once

#include <cstddef>
#include <functional>

namespace flux {

// Coarse progress for long runs. Called from the thread that runs indexing/sync, never from
// workers, so the callback needs no locking.
//   stage "scanned":  `done` = eligible files found, `total` = 0
//   stage "processed": `done` files handed to workers so far, `total` = all eligible files
using ProgressFn = std::function<void(const char* stage, std::size_t done, std::size_t total)>;

// How often (in files) the "processed" stage is reported.
constexpr std::size_t kProgressInterval = 2000;

}  // namespace flux
