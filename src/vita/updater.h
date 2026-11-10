// Installs a downloaded update VPK over the app's own files, then restarts the app.
#pragma once

#include <atomic>
#include <string>

namespace vt {

// Where the running app is installed (VitaShell installs homebrew under ux0:app/<TITLE ID>).
constexpr const char* kAppDir = "ux0:app/VIVO00001";

// Unpacks every file of the VPK (except sce_sys/, which the system keeps track of) next to the
// current ones, checks each file's CRC, and only then swaps them in. `done` / `total` report
// progress in files. Blocks; run it on a worker thread.
bool install_update(const std::string& vpk, const std::string& appDir, std::string* err,
                    std::atomic<int>* done = nullptr, std::atomic<int>* total = nullptr);

// Whether the app may replace its own files. HENkaku only lets homebrew write to ux0:app when
// Settings > HENkaku Settings > Enable Unsafe Homebrew is on (it's off by default); without it,
// updates have to be installed with VitaShell.
bool can_update_in_place(const std::string& appDir);

// Starts app0:eboot.bin again (the newly installed one). Returns false if the system refused.
bool relaunch_app();

}  // namespace vt
