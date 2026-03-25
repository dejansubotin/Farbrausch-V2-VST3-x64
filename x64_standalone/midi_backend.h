#pragma once

#include <windows.h>

#include "../types.h"

class file;

#include "../vsti/midi.h"

bool msOpenStandaloneAudio(HWND hwnd);
void msCloseStandaloneAudio();
bool msOpenPreferredMidiInput();
void msClosePreferredMidiInput();
