// SPDX-License-Identifier: GPL-3.0-or-later
// Process-local exclusion of Overwolf's Vulkan layers (see vk_layer_filter.cpp).
#pragma once

#include <map>
#include <set>
#include <string>

namespace nxrt::gfx {

// Reads one Vulkan layer manifest (JSON text). When the manifest's path, a layer's name or its library_path names Overwolf ("overwolf",
// "ow-graphics"), adds the layer's name to `names` and the variables of its "disable_environment" object to `environment` (an empty
// value becomes "1": the loader needs a non-empty one). Other layers, and text that is not a manifest, add nothing.
void collect_overwolf_layers(const std::string& manifest_text, const std::string& manifest_path, std::set<std::string>& names,
                             std::map<std::string, std::string>& environment);

// Windows: once per process, before the Vulkan loader is opened, disables the installed Overwolf layers for this process only
// ([gfx] disable_overwolf_layers, default true). Registry entries and files are not touched. Does nothing elsewhere.
void disable_overwolf_vulkan_layers();

}  // namespace nxrt::gfx
