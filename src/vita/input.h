// Buttons (with auto-repeat), analog sticks and front touch, sampled once per frame.
#pragma once

#include <cstdint>

namespace vt {

enum Button : uint32_t {
    BTN_SELECT = 0x000001,
    BTN_START = 0x000008,
    BTN_UP = 0x000010,
    BTN_RIGHT = 0x000020,
    BTN_DOWN = 0x000040,
    BTN_LEFT = 0x000080,
    BTN_L = 0x000100,
    BTN_R = 0x000200,
    BTN_TRIANGLE = 0x001000,
    BTN_CIRCLE = 0x002000,
    BTN_CROSS = 0x004000,
    BTN_SQUARE = 0x008000,
    // Logical buttons, resolved from the system "enter button" setting.
    BTN_CONFIRM = 0x10000000,
    BTN_CANCEL = 0x20000000,
};

struct Input {
    void init();
    void poll();

    bool pressed(uint32_t b) const { return (pressed_ & b) != 0; }
    bool held(uint32_t b) const { return (held_ & b) != 0; }
    // Pressed this frame, or auto-repeating while held (for list navigation).
    bool repeat(uint32_t b) const { return (repeat_ & b) != 0; }
    void consume(uint32_t b) {
        pressed_ &= ~b;
        repeat_ &= ~b;
    }
    // For the next few polls, buttons that are down count as already held (no new presses).
    // Used after system dialogs close: a button held when the dialog went away (or a key-up the
    // dialog swallowed, in Vita3K) must not act on the screen underneath.
    void settle(int frames) { settleFrames_ = frames; }

    // Touch, in 960x544 screen coordinates.
    bool touching = false;
    bool touchBegan = false, touchEnded = false;
    bool tapped = false;    // released quickly without moving: a tap at (tapX, tapY)
    bool dragging = false;  // moved far enough to count as a drag
    float tx = 0, ty = 0, startX = 0, startY = 0, dx = 0, dy = 0, tapX = 0, tapY = 0;
    float velocityY = 0;    // px per frame, smoothed, for fling scrolling
    void consumeTap() { tapped = false; }
    bool tapIn(float x, float y, float w, float h) const {
        return tapped && tapX >= x && tapX < x + w && tapY >= y && tapY < y + h;
    }
    bool touchStartedIn(float x, float y, float w, float h) const {
        return startX >= x && startX < x + w && startY >= y && startY < y + h;
    }

    int lx = 128, ly = 128, rx = 128, ry = 128;
    bool confirmIsCircle = false;

private:
    uint32_t held_ = 0, pressed_ = 0, repeat_ = 0;
    uint32_t repeatMask_ = 0;
    int settleFrames_ = 0;
    double repeatAt_ = 0;
    double touchStartTime_ = 0;
};

}  // namespace vt
