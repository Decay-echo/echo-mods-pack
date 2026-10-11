#pragma once
#include <string>
#include <windows.h>

// Oculus-runtime tweaks (FOV multiplier). Call once, as early as possible.
namespace tweaks {
void start(const std::wstring& pluginDir);
}
