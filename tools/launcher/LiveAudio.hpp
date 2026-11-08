#pragma once

#include <string>

namespace launcher {

unsigned long FindRunningTitle(const std::wstring& executablePath);

bool PushLiveAudio(unsigned long processId, float bgmGain, float mainGain, float musicFloor);

}  // namespace launcher
