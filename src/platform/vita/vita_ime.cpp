// The Vita's system keyboard dialog (VITA-55). See include/platform/vita/ime_dialog.hpp.
#include "platform/vita/ime_dialog.hpp"

#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>

#include "core/logger.hpp"

#include <cstdint>
#include <string>

namespace wowee::platform::vita {

namespace {

constexpr int kMaxChars = 512;  // enough for a name, a password or a chat line; the dialog's cap is 2048

SceWChar16 g_initial[kMaxChars + 1];
SceWChar16 g_buffer[kMaxChars + 1];
const SceWChar16 g_title[1] = {0};
bool g_active = false;
bool g_requested = false;
bool g_requestPassword = false;
std::string g_requestText;

// UTF-8 <-> UTF-16 (the dialog's SceWChar16), surrogate pairs included.
void toUtf16(const std::string& in, SceWChar16* out, int cap) {
    int n = 0;
    for (std::size_t i = 0; i < in.size() && n < cap;) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        uint32_t cp = 0;
        int len = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 0x6 && i + 1 < in.size()) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < in.size()) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < in.size()) { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
        i += len;
        if (cp >= 0x10000) {
            if (n + 2 > cap) break;
            cp -= 0x10000;
            out[n++] = static_cast<SceWChar16>(0xD800 + (cp >> 10));
            out[n++] = static_cast<SceWChar16>(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = static_cast<SceWChar16>(cp);
        }
    }
    out[n] = 0;
}

std::string toUtf8(const SceWChar16* in) {
    std::string out;
    for (int i = 0; in[i] != 0 && i < kMaxChars; ++i) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp < 0xDC00 && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace

void imeRequest(const std::string& initialUtf8, bool password) {
    g_requested = true;
    g_requestPassword = password;
    g_requestText = initialUtf8;
}

bool imeTakeRequest(std::string& initialUtf8, bool& password) {
    if (!g_requested) return false;
    g_requested = false;
    initialUtf8 = g_requestText;
    password = g_requestPassword;
    return true;
}

bool imeOpen(const std::string& initialUtf8, bool password) {
    if (g_active) return false;
    toUtf16(initialUtf8, g_initial, kMaxChars);
    g_buffer[0] = 0;

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0;
    param.languagesForced = SCE_FALSE;
    param.type = 0;  // SCE_IME_TYPE_DEFAULT
    param.option = 0;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = g_title;
    param.maxTextLength = kMaxChars;
    param.initialText = g_initial;
    param.inputTextBuffer = g_buffer;
    const int rc = sceImeDialogInit(&param);
    if (rc < 0) {
        LOG_WARNING("sceImeDialogInit failed: 0x", std::hex, static_cast<unsigned>(rc));
        return false;
    }
    g_active = true;
    return true;
}

bool imeActive() { return g_active; }

bool imePoll(bool& confirmed, std::string& textUtf8) {
    if (!g_active) return false;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return false;
    SceImeDialogResult result{};
    sceImeDialogGetResult(&result);
    confirmed = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
    textUtf8 = confirmed ? toUtf8(g_buffer) : std::string();
    sceImeDialogTerm();
    g_active = false;
    return true;
}

}  // namespace wowee::platform::vita
