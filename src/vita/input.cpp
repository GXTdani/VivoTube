#include "input.h"

#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>

#include <cmath>

#include "../core/util.h"

namespace vt {

void Input::init() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter) == 0)
        confirmIsCircle = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
}

void Input::poll() {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = pad.buttons & 0xFFFF;
    lx = pad.lx;
    ly = pad.ly;
    rx = pad.rx;
    ry = pad.ry;
    // The left stick doubles as a d-pad.
    if (ly < 40) b |= BTN_UP;
    if (ly > 216) b |= BTN_DOWN;
    if (lx < 40) b |= BTN_LEFT;
    if (lx > 216) b |= BTN_RIGHT;
    if (b & (confirmIsCircle ? BTN_CIRCLE : BTN_CROSS)) b |= BTN_CONFIRM;
    if (b & (confirmIsCircle ? BTN_CROSS : BTN_CIRCLE)) b |= BTN_CANCEL;

    double now = monotonic_seconds();
    if (settleFrames_ > 0) {
        // Treat whatever is down as already held; nothing counts as newly pressed.
        settleFrames_--;
        held_ = b;
        pressed_ = repeat_ = 0;
        repeatMask_ = 0;
    } else {
        pressed_ = b & ~held_;
        held_ = b;
        repeat_ = pressed_;
        const uint32_t repeatable = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_L | BTN_R;
        if (pressed_ & repeatable) {
            repeatMask_ = pressed_ & repeatable;
            repeatAt_ = now + 0.38;
        } else if ((held_ & repeatMask_) && now >= repeatAt_) {
            repeat_ |= held_ & repeatMask_;
            repeatAt_ = now + 0.075;
        } else if (!(held_ & repeatMask_)) {
            repeatMask_ = 0;
        }
    }

    SceTouchData touch{};
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    bool now_touching = touch.reportNum > 0;
    float x = now_touching ? touch.report[0].x / 2.0f : tx;
    float y = now_touching ? touch.report[0].y / 2.0f : ty;
    touchBegan = now_touching && !touching;
    touchEnded = !now_touching && touching;
    tapped = false;
    if (touchBegan) {
        startX = x;
        startY = y;
        tx = x;
        ty = y;
        dragging = false;
        velocityY = 0;
        touchStartTime_ = now;
    }
    dx = now_touching && !touchBegan ? x - tx : 0;
    dy = now_touching && !touchBegan ? y - ty : 0;
    if (now_touching) {
        if (std::fabs(x - startX) > 12 || std::fabs(y - startY) > 12) dragging = true;
        velocityY = velocityY * 0.6f + dy * 0.4f;
        tx = x;
        ty = y;
    }
    if (touchEnded) {
        if (!dragging && now - touchStartTime_ < 0.6) {
            tapped = true;
            tapX = tx;
            tapY = ty;
        }
    }
    touching = now_touching;
}

}  // namespace vt
