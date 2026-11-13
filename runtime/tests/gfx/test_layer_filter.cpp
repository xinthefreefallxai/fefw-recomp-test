// SPDX-License-Identifier: GPL-3.0-or-later
// vk_layer_filter.cpp: which Vulkan layer manifests are Overwolf's and what they ask the loader to disable them with. Pure text, no GPU.
#include <map>
#include <set>
#include <string>

#include "test_framework.h"

#include "vk_layer_filter.h"

NXTEST_GROUP(layer_filter)

using nxrt::gfx::collect_overwolf_layers;

NXTEST(overwolf_manifest_gives_name_and_disable_control) {
    std::set<std::string> names;
    std::map<std::string, std::string> env;
    collect_overwolf_layers(R"({"file_format_version":"1.0.0","layer":{"name":"VK_LAYER_OW_OBS_HOOK","library_path":"ow-graphics-hook64.dll",
        "disable_environment":{"DISABLE_OW_TEST":"1"}}})", "C:/layers/overlay.json", names, env);
    CHECK(names.count("VK_LAYER_OW_OBS_HOOK") == 1);
    CHECK(env.count("DISABLE_OW_TEST") == 1 && env["DISABLE_OW_TEST"] == "1");
}

NXTEST(other_overlays_are_left_alone) {
    std::set<std::string> names;
    std::map<std::string, std::string> env;
    collect_overwolf_layers(R"({"layer":{"name":"VK_LAYER_OBS_HOOK","library_path":"graphics-hook64.dll",
        "disable_environment":{"DISABLE_OBS_TEST":"1"}}})", "C:/OBS/overlay.json", names, env);
    CHECK(names.empty());
    CHECK(env.empty());
}

NXTEST(layers_array_path_match_and_bad_fields) {
    std::set<std::string> names;
    std::map<std::string, std::string> env;
    // the path names Overwolf: every layer of the manifest counts; an empty value becomes "1"; bad names, types and keys are skipped
    collect_overwolf_layers(R"({"layers":[
        {"name":"VK_LAYER_CAPTURE","disable_environment":{"DISABLE_CAPTURE_TEST":""}},
        {"name":42,"disable_environment":false},
        {"name":"VK_LAYER_OTHER","disable_environment":{"BAD=NAME":"1","BAD_TYPE":2}}]})", "C:/OVERWOLF/layers.json", names, env);
    CHECK(names.size() == 2);
    CHECK(env.size() == 1 && env["DISABLE_CAPTURE_TEST"] == "1");
}

NXTEST(invalid_text_adds_nothing) {
    std::set<std::string> names;
    std::map<std::string, std::string> env;
    collect_overwolf_layers("{invalid", "C:/OVERWOLF/x.json", names, env);
    collect_overwolf_layers("", "C:/OVERWOLF/x.json", names, env);
    collect_overwolf_layers("[1,2,3]", "C:/OVERWOLF/x.json", names, env);
    collect_overwolf_layers(R"({"layer":{"name":"VK_LAYER_OW_A","disable_environment":{"K":"v")", "C:/OVERWOLF/x.json", names, env);   // cut off
    CHECK(names.empty());
    CHECK(env.empty());
}

NXTEST(escapes_and_repeat_discovery) {
    std::set<std::string> names;
    std::map<std::string, std::string> env;
    const std::string m = R"({"layer":{"name":"VK_LAYER_OW_\u0041","library_path":"C:\Overwolf\x.dll","disable_environment":{"DISABLE_A":"1"}}})";
    collect_overwolf_layers(m, "x.json", names, env);
    collect_overwolf_layers(m, "x.json", names, env);
    CHECK(names.size() == 1 && names.count("VK_LAYER_OW_A") == 1);
    CHECK(env.size() == 1);
}
