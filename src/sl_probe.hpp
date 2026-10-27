#pragma once
#include <filesystem>
#include <string>
namespace sl_probe {
// Hooks Streamline once sl.interposer.dll is loaded (Poll). The hooks always run,
// because the star composite depends on them; with diagnostics off no metadata log,
// pixel snapshots or files are produced.
void Start(const std::filesystem::path &directory, bool diagnostics);
void Poll();
// One-time hook installation result for ReShade.log, empty until Poll has attached.
std::string TakeStatus();
void Stop();
}
