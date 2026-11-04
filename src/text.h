// The app's language: English (Portuguese() is false; the Portuguese strings
// are kept for a translation later).

#pragma once

namespace text {

bool Portuguese();
inline const char* T(const char* english, const char* portuguese) { return Portuguese() ? portuguese : english; }

}  // namespace text
