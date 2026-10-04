// Padding for the read-only segment of wowee_client (VITA-17): see the comment in cmake/vita/wowee_client.cmake. The Vita's
// ELF tooling needs room after the end of the text segment for its import data, and the room left is whatever the code size
// happens to leave before the next 64 KB boundary.
#ifndef WOWEE_VITA_TEXT_PAD_KB
#define WOWEE_VITA_TEXT_PAD_KB 56
#endif

extern "C" {
// Declared extern first: a const object has internal linkage in C++ otherwise, and the link option -u would find nothing.
extern const unsigned char wowee_vita_text_pad[WOWEE_VITA_TEXT_PAD_KB * 1024];
// const, so it lands in .rodata inside the first (read-only, executable) segment; kept by -u in the link options.
__attribute__((used)) const unsigned char wowee_vita_text_pad[WOWEE_VITA_TEXT_PAD_KB * 1024] = {0};
}
