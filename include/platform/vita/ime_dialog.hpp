#pragma once
// The Vita's system keyboard (sceImeDialog), opened by the client itself with the field's current text as the
// initial text (VITA-55). SDL's own Vita keyboard cannot do that: its dialog always opens empty.

#include <string>

namespace wowee::platform::vita {

/// A text field was tapped: ask for the dialog on the next frame, with the field's text and whether it is a password.
/// Called from PaperUI::field (the pre-game screens' own fields, which ImGui knows nothing about).
void imeRequest(const std::string& initialUtf8, bool password);

/// The pending request, if any (taken: it is returned once).
bool imeTakeRequest(std::string& initialUtf8, bool& password);

/// Open the dialog with `initialUtf8` in the box. False if one is already open or the system refused.
bool imeOpen(const std::string& initialUtf8, bool password);

/// While the dialog is open the frame must be presented with vglSwapBuffers(GL_TRUE), or it is not drawn.
[[nodiscard]] bool imeActive();

/// Poll once per frame. True when the dialog just closed; `confirmed` is whether the player pressed Enter, and
/// `textUtf8` the box content then.
bool imePoll(bool& confirmed, std::string& textUtf8);

}  // namespace wowee::platform::vita
