#pragma once

#include <string>

namespace flux::gui {

// Drives the real window (created off-screen) through its own code paths and checks what the real
// controls show. Writes `smoke-result.txt` into `workDirectory` and returns the number of failed
// checks (0 = everything passed). `screenshotDirectory` (optional) receives BMP screenshots.
// All data lives under `workDirectory`; the user's database and settings are never used.
int RunSmokeTest(const std::wstring& workDirectory, const std::wstring& screenshotDirectory);

}  // namespace flux::gui
