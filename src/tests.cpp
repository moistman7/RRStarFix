// Offline callback tests: no game, injection, GPU, or ReShade runtime required.
#define RRSTARFIX_OFFLINE
#include "addon.cpp"
#include <iostream>
#include <stdexcept>
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
  try {
    Check(argc == 2, "Test output path required");
    output = fs::path(argv[1]) / ("test-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(Hash("", 0) == 14695981039346656037ull, "Empty FNV vector failed");
    Check(Hash("hello", 5) == 0xa430d84680aabd0bull, "FNV known vector failed");
    // RRStarFix.ini: defaults written on first run, smoothing saved, bad values rejected.
    fs::create_directories(output);
    const auto ini_path = output / "RRStarFix.ini";
    std::vector<std::string> warnings;
    auto loaded = config::Load(ini_path, warnings);
    Check(fs::exists(ini_path) && warnings.empty(), "Default RRStarFix.ini not created");
    Check(loaded.smoothing == 1 && !loaded.diagnostics &&
          loaded.smoothing_key == VK_F7 && loaded.snapshot_key == VK_F9 && loaded.trace_key == VK_F10, "Ini defaults wrong");
    Check(config::SaveState(ini_path, 2), "Saving smoothing failed");
    loaded = config::Load(ini_path, warnings);
    Check(loaded.smoothing == 2 && warnings.empty(), "Saved smoothing not reloaded");
    {
      std::ifstream file(ini_path); const std::string text((std::istreambuf_iterator<char>(file)), {});
      Check(text.find("; hotkeys:") != std::string::npos && text.find("Smoothing=strong") != std::string::npos, "Saving dropped the ini comments");
    }
    WritePrivateProfileStringW(L"RRStarFix", L"StarComposite", L"0", ini_path.c_str());
    WritePrivateProfileStringW(L"RRStarFix", L"ToggleKey", L"Banana", ini_path.c_str());
    WritePrivateProfileStringW(L"RRStarFix", L"Diagnostics", L"yes", ini_path.c_str());
    WritePrivateProfileStringW(L"RRStarFix", L"Smoothing", L"max", ini_path.c_str());
    WritePrivateProfileStringW(L"RRStarFix", L"SmoothingKey", L"PageDown", ini_path.c_str());
    loaded = config::Load(ini_path, warnings);
    Check(warnings.size() == 2 && !loaded.diagnostics && loaded.smoothing == 1 &&
          loaded.smoothing_key == VK_NEXT, "Invalid ini values not rejected with warnings");
    Check(config::ParseKey("f8") == VK_F8 && config::ParseKey("PageUp") == VK_PRIOR && config::ParseKey("0x77") == 0x77 &&
          config::ParseKey("None") == 0 && config::ParseKey("") == 0 && config::ParseKey("x") == 'X' &&
          config::ParseKey("NumPad5") == VK_NUMPAD5 && config::ParseKey("F24") == VK_F24 && config::ParseKey("F25") == -1 &&
          config::ParseKey("0xZZ") == -1 && config::ParseKey("0x100") == -1, "Key name parsing wrong");
    Check(config::KeyName(VK_F10) == "F10" && config::KeyName(VK_PRIOR) == "PageUp" && config::KeyName(0) == "None" &&
          config::KeyName('Q') == "Q", "Key display names wrong");
    const char bytes[] = "DXBC-offline-test";
    api::shader_desc desc; desc.code = bytes; desc.code_size = sizeof(bytes);
    api::pipeline_subobject object{api::pipeline_subobject_type::pixel_shader, 1, &desc};
    InitPipeline(nullptr, {}, 1, &object, api::pipeline{42});
    InitPipeline(nullptr, {}, 1, &object, api::pipeline{43});
    Check(shaders.size() == 1, "Shader deduplication failed");
    Check(pipelines.size() == 2, "Pipeline inventory failed");
    const auto original_bytes = cached_bytes;
    cached_bytes = max_shader_bytes;
    const char extra[] = "DXBC-other"; desc.code = extra; desc.code_size = sizeof(extra);
    InitPipeline(nullptr, {}, 1, &object, api::pipeline{44});
    Check(shaders.size() == 1 && dropped_shaders == 1, "Shader memory cap failed");
    cached_bytes = original_bytes;
    auto *fake = reinterpret_cast<api::command_list *>(uintptr_t{123});
    recording.store(true);
    bindings[fake] = pipelines.at({nullptr, 42});
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i) producers.emplace_back([fake] {
      for (int j = 0; j < 1000; ++j) Check(!Record(fake, 1, 3, 1, 0), "Draw suppressed");
    });
    for (auto &t : producers) t.join();
    Check(events.size() == 8000, "Concurrent event capture failed");
    Check(events[0].ps == Hash(bytes, sizeof(bytes)), "Shader association failed");
    ResetCommand(fake); Check(bindings.empty(), "Command reset failed");
    FinishCapture(); if (writer.joinable()) writer.join();
    Check(!writing.load() && !recording.load(), "Capture did not finish");
    Check(fs::exists(output), "Capture directory missing");
    bool trace_found = false;
    for (const auto &entry : fs::directory_iterator(output)) {
      if (!entry.is_directory()) continue;
      std::ifstream trace(entry.path() / "trace.csv");
      std::string line; size_t rows = 0;
      while (std::getline(trace, line)) ++rows;
      if (rows == 8001) trace_found = true;
    }
    Check(trace_found, "Expected 8000 trace records plus CSV header");
    events.resize(max_events); recording.store(true);
    const auto before = dropped_events.load();
    Check(!Record(fake, 0, 3, 1, 0), "Overflow suppressed draw");
    Check(events.size() == max_events && dropped_events.load() == before + 1, "Event cap failed");
    recording.store(false); events.clear();
    DestroyPipeline(nullptr, api::pipeline{42});
    Check(!pipelines.contains({nullptr, 42}), "Pipeline destruction failed");
    std::cout << "PASS: hash vectors, deduplication, memory/event caps, concurrent tracing,\n"
                 "shader association, command reset, capture files, non-suppression, destruction, ini settings and key names.\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
