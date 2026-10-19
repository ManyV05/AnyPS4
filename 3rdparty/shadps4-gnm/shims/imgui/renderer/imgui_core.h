// AnyPS4 shim: no Dear ImGui overlay.
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace ImGui::Core {
inline void Shutdown(const vk::Device&) {}
namespace TextureManager {
inline void Submit() {}
} // namespace TextureManager
} // namespace ImGui::Core
