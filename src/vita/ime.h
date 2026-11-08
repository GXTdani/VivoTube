// System on-screen keyboard (SceImeDialog). The dialog is drawn by the system; the app must
// call vita2d_common_dialog_update() every frame while it is open.
#pragma once

#include <cstdint>
#include <string>

namespace vt {

class Ime {
public:
    bool open(const std::string& title, const std::string& initial, int maxLength = 128);
    // 0 = still open, 1 = confirmed (text in out), -1 = cancelled.
    int poll(std::string& out);
    bool active() const { return active_; }

private:
    bool active_ = false;
    uint16_t title_[64] = {};
    uint16_t initial_[256] = {};
    uint16_t buffer_[256] = {};
};

}  // namespace vt
