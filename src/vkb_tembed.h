#pragma once
#ifdef BOARD_TEMBED
#include <Arduino.h>

// Blocking encoder-driven virtual keyboard for T-Embed.
// Encoder scroll navigates the character grid; click selects; side button deletes / cancels.
// Returns the entered string. If *cancelled is provided, it is set true when the user
// presses Back on an empty input (i.e. they chose not to enter anything).
String vkbInput(const char* prompt, const char* initial = "",
                int maxLen = 64, bool* cancelled = nullptr);
#endif
