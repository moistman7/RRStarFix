#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace config {
// Settings from RRStarFix.ini next to the add-on.
struct Settings {
  unsigned smoothing=1;       // Smoothing: 0 light, 1 medium, 2 strong
  bool diagnostics=false;     // Diagnostics: metadata log, F9 snapshots, F10 trace
  int smoothing_key=0x76;     // F7
  int snapshot_key=0x78;      // F9 (diagnostics only)
  int trace_key=0x79;         // F10 (diagnostics only)
};
// Reads the file, writing a commented default file first if none exists.
// Problems (unreadable values, unwritable folder) are appended to `warnings`.
Settings Load(const std::filesystem::path &ini, std::vector<std::string> &warnings);
// Saves the smoothing choice so they survive a restart; false if the file cannot be written.
bool SaveState(const std::filesystem::path &ini, unsigned smoothing);
// Key names: F1-F24, A-Z, 0-9, PageUp, PageDown, Home, End, Insert, Delete, Pause,
// ScrollLock, NumPad0-NumPad9, or a hex virtual-key code such as 0x77. "None" or empty
// disables the key (returns 0). Unknown names return -1.
int ParseKey(const std::string &name);
std::string KeyName(int key);
const char *SmoothingName(unsigned level);
}
