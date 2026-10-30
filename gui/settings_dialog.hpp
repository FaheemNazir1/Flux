#pragma once

#include "win_util.hpp"

#include "flux/gui_settings.hpp"

namespace flux::gui {

// Shows the modal settings dialog. `settings` is edited in place only if the user presses OK and the
// values are valid; returns whether it was changed. `defaultDatabase` is shown as the location that
// "use the default" refers to.
bool ShowSettingsDialog(HWND owner, HFONT font, UINT dpi, GuiSettings& settings, const std::wstring& defaultDatabase);

}  // namespace flux::gui
