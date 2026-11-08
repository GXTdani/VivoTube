#include "ime.h"

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>

#include <cstring>

#include "../core/util.h"

namespace vt {

namespace {

void utf8_to_utf16(const std::string& s, uint16_t* out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < s.size() && n + 2 < cap; i = utf8_next(s, i)) {
        uint32_t cp = utf8_decode(s, i);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    out[n] = 0;
}

std::string utf16_to_utf8(const uint16_t* s) {
    std::string out;
    for (size_t i = 0; s[i]; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace

bool Ime::open(const std::string& title, const std::string& initial, int maxLength) {
    if (active_) return false;
    utf8_to_utf16(title, title_, sizeof(title_) / 2);
    utf8_to_utf16(initial, initial_, sizeof(initial_) / 2);
    memset(buffer_, 0, sizeof(buffer_));
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0x0001FFFF;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = (const SceWChar16*)title_;
    p.maxTextLength = (SceUInt32)(maxLength < 255 ? maxLength : 254);
    p.initialText = (SceWChar16*)initial_;
    p.inputTextBuffer = (SceWChar16*)buffer_;
    int rc = sceImeDialogInit(&p);
    if (rc < 0) {
        VT_LOG("ime: sceImeDialogInit failed 0x%08X", (unsigned)rc);
        return false;
    }
    active_ = true;
    return true;
}

int Ime::poll(std::string& out) {
    if (!active_) return -1;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
    SceImeDialogResult r;
    memset(&r, 0, sizeof(r));
    sceImeDialogGetResult(&r);
    sceImeDialogTerm();
    active_ = false;
    if (r.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        out = utf16_to_utf8(buffer_);
        return 1;
    }
    return -1;
}

}  // namespace vt
