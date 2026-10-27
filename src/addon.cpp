// RRStarFix: restores stars after DLSS Ray Reconstruction. Settings in RRStarFix.ini; diagnostics off by default.
#include <windows.h>
#include <reshade.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>
#include "sl_probe.hpp"
#include "gpu_snapshot.hpp"
#include "star_composite.hpp"
#include "config.hpp"

namespace fs = std::filesystem;
namespace api = reshade::api;
namespace {
constexpr size_t max_shader_bytes = 64u * 1024u * 1024u;
constexpr size_t max_shader_count = 8192;
constexpr size_t max_events = 250000;
constexpr unsigned capture_frames = 120;
struct Shader { uint64_t hash; unsigned stage; std::vector<uint8_t> code; };
struct Pipeline { uint64_t vs = 0, ps = 0, cs = 0; };
struct Event { uint64_t frame, command, vs, ps, cs; unsigned kind, x, y, z; };
std::mutex mutex;
std::map<uint64_t, Shader> shaders;
std::map<std::pair<api::device *, uint64_t>, Pipeline> pipelines;
std::unordered_map<api::command_list *, Pipeline> bindings;
std::vector<Event> events;
std::atomic<bool> recording{false}, writing{false};
uint64_t frame = 0, first_frame = 0;
size_t cached_bytes = 0, dropped_shaders = 0;
std::atomic<size_t> dropped_events{0};
fs::path output;
// Heap-held and never destroyed: if ReShade skips AddonUninit, a joinable thread
// destroyed at process exit would call std::terminate.
std::thread &writer = *new std::thread;
bool smoothing_down = false, snapshot_down = false, trace_down = false, save_warned = false;
config::Settings settings;
fs::path ini;

uint64_t Hash(const void *data, size_t size) {
  auto p = static_cast<const uint8_t *>(data);
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
std::string Hex(uint64_t n) {
  std::ostringstream s; s << std::hex << std::setw(16) << std::setfill('0') << n; return s.str();
}
void Log(const char *s) {
#ifndef RRSTARFIX_OFFLINE
  reshade::log::message(reshade::log::level::info, s);
#else
  (void)s;
#endif
}
// F10 trace support (diagnostics only): shader inventory per pipeline.
void InitPipeline(api::device *device, api::pipeline_layout, uint32_t count,
                  const api::pipeline_subobject *objects, api::pipeline handle) {
  try {
    std::lock_guard lock(mutex);
    Pipeline p;
    for (uint32_t i = 0; i < count; ++i) {
      const auto type = objects[i].type;
      if (type != api::pipeline_subobject_type::vertex_shader &&
          type != api::pipeline_subobject_type::pixel_shader &&
          type != api::pipeline_subobject_type::compute_shader) continue;
      if (!objects[i].data || objects[i].count != 1) continue;
      const auto &s = *static_cast<const api::shader_desc *>(objects[i].data);
      if (!s.code || !s.code_size) continue;
      const auto h = Hash(s.code, s.code_size);
      if (type == api::pipeline_subobject_type::vertex_shader) p.vs = h;
      if (type == api::pipeline_subobject_type::pixel_shader) p.ps = h;
      if (type == api::pipeline_subobject_type::compute_shader) p.cs = h;
      if (shaders.contains(h)) continue;
      if (shaders.size() >= max_shader_count || s.code_size > max_shader_bytes - cached_bytes) {
        ++dropped_shaders; continue;
      }
      const auto *bytes = static_cast<const uint8_t *>(s.code);
      shaders.emplace(h, Shader{h, static_cast<unsigned>(type), {bytes, bytes + s.code_size}});
      cached_bytes += s.code_size;
    }
    if (pipelines.size() < 32768 || pipelines.contains({device, handle.handle}))
      pipelines[{device, handle.handle}] = p;
  } catch (...) { Log("[RRStarFix] Shader inventory callback failed; rendering unchanged."); }
}
void DestroyPipeline(api::device *d, api::pipeline p) {
  std::lock_guard lock(mutex); pipelines.erase({d, p.handle});
}
void BindPipeline(api::command_list *cmd, api::pipeline_stage stages, api::pipeline p) {
  if (!recording.load(std::memory_order_relaxed)) return;
  try {
    std::lock_guard lock(mutex);
    auto it = pipelines.find({cmd->get_device(), p.handle});
    const Pipeline value = it == pipelines.end() ? Pipeline{} : it->second;
    if (bindings.size() >= 4096 && !bindings.contains(cmd)) { ++dropped_events; return; }
    auto &b = bindings[cmd];
    if (static_cast<uint32_t>(stages & api::pipeline_stage::vertex_shader)) b.vs = value.vs;
    if (static_cast<uint32_t>(stages & api::pipeline_stage::pixel_shader)) b.ps = value.ps;
    if (static_cast<uint32_t>(stages & api::pipeline_stage::compute_shader)) b.cs = value.cs;
  } catch (...) { ++dropped_events; }
}
// Bindings are only tracked during an F10 recording, and each recording starts with
// an empty map, so outside a recording there is nothing to erase and no lock to take.
void ResetCommand(api::command_list *cmd) {
  if (!recording.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex); bindings.erase(cmd);
}
bool Record(api::command_list *cmd, unsigned kind, unsigned x, unsigned y, unsigned z) {
  if (!recording.load(std::memory_order_relaxed)) return false;
  try {
    std::lock_guard lock(mutex);
    if (!recording.load()) return false;
    if (events.size() >= max_events) { ++dropped_events; return false; }
    auto b = bindings.find(cmd);
    const auto p = b == bindings.end() ? Pipeline{} : b->second;
    events.push_back({frame, reinterpret_cast<uintptr_t>(cmd), p.vs, p.ps, p.cs, kind, x, y, z});
  } catch (...) { ++dropped_events; }
  return false; // Always allow the original draw/dispatch.
}
bool Draw(api::command_list *c, uint32_t n, uint32_t instances, uint32_t, uint32_t) {
  return Record(c, 0, n, instances, 0);
}
bool DrawIndexed(api::command_list *c, uint32_t n, uint32_t instances, uint32_t, int32_t, uint32_t) {
  return Record(c, 1, n, instances, 0);
}
bool Dispatch(api::command_list *c, uint32_t x, uint32_t y, uint32_t z) { return Record(c, 2, x, y, z); }
void FinishCapture() {
  recording.store(false);
  if (writer.joinable()) writer.join();
  std::vector<Event> captured;
  std::map<uint64_t, Shader> inventory;
  size_t missed_shaders, missed_events;
  {
    std::lock_guard lock(mutex);
    captured.swap(events); inventory = shaders;
    missed_shaders = dropped_shaders; missed_events = dropped_events.load();
    bindings.clear();
  }
  writing.store(true);
  try {
    writer = std::thread([captured = std::move(captured), inventory = std::move(inventory),
                          missed_shaders, missed_events]() {
      try {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto directory = output / ("capture-" + std::to_string(stamp));
        fs::create_directories(directory / "shaders");
        std::ofstream manifest(directory / "shaders.csv");
        manifest << "fnv1a64,stage,bytes\n";
        for (const auto &[hash, s] : inventory) {
          const auto name = Hex(hash);
          std::ofstream file(directory / "shaders" / (name + ".bin"), std::ios::binary);
          file.write(reinterpret_cast<const char *>(s.code.data()), s.code.size());
          if (!file) throw std::runtime_error("Shader write failed");
          manifest << name << ',' << s.stage << ',' << s.code.size() << '\n';
        }
        std::ofstream trace(directory / "trace.csv");
        trace << "present_frame,command_list,vs,ps,cs,kind,count_x,instances_y,z\n";
        for (const auto &e : captured)
          trace << e.frame << ',' << Hex(e.command) << ',' << Hex(e.vs) << ',' << Hex(e.ps)
                << ',' << Hex(e.cs) << ',' << e.kind << ',' << e.x << ',' << e.y << ',' << e.z << '\n';
        std::ofstream summary(directory / "summary.txt");
        summary << "RRStarFix F10 draw/dispatch trace.\n"
                << "Events: " << captured.size() << "\nShaders: " << inventory.size()
                << "\nDropped shader entries: " << missed_shaders << "\nDropped events: " << missed_events
                << "\nFrame numbers mark CPU recording/present epochs, not GPU execution order.\n"
                << "No screenshots, textures, constant buffers, or save data collected.\n";
        if (!trace || !manifest || !summary) throw std::runtime_error("Capture write failed");
        Log("[RRStarFix] Capture saved under LOCALAPPDATA/RRStarFix/Captures.");
      } catch (...) { Log("[RRStarFix] Capture write failed; check output permissions/free space."); }
      writing.store(false);
    });
  } catch (...) { writing.store(false); Log("[RRStarFix] Could not start capture writer."); }
}
// A key press edge while the game window is focused; key 0 means disabled.
bool Pressed(int key, bool &down, bool focused) {
  const bool now = key > 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
  const bool edge = now && !down && focused;
  down = now;
  return edge;
}
void Present(api::command_queue *queue, api::swapchain *, const api::rect *, const api::rect *, uint32_t, const api::rect *) {
  try {
#ifndef RRSTARFIX_OFFLINE
    sl_probe::Poll();
    if (const auto status = sl_probe::TakeStatus(); !status.empty()) Log(status.c_str());
    if (settings.diagnostics && queue && queue->get_device()->get_api() == api::device_api::d3d12)
      gpu_snapshot::Present(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
    star_composite::Present();
    for (const auto &message : star_composite::TakeMessages()) Log(message.c_str());
#else
    (void)queue;
#endif
    { std::lock_guard lock(mutex); ++frame; }
    DWORD process = 0; GetWindowThreadProcessId(GetForegroundWindow(), &process);
    const bool focused = process == GetCurrentProcessId();
#ifndef RRSTARFIX_OFFLINE
    bool changed = false;
    if (Pressed(settings.smoothing_key, smoothing_down, focused)) { Log(star_composite::CycleSmoothing().c_str()); changed = true; }
    // Remember the choices for the next launch.
    if (changed && !config::SaveState(ini, star_composite::Smoothing()) && !save_warned) {
      save_warned = true; Log("[RRStarFix] Could not save settings to RRStarFix.ini; they will reset next launch.");
    }
    if (!settings.diagnostics) return;
    if (Pressed(settings.snapshot_key, snapshot_down, focused)) Log(("[RRStarFix] " + gpu_snapshot::Arm()).c_str());
#endif
    const bool trigger = Pressed(settings.trace_key, trace_down, focused);
    if (recording.load() && frame - first_frame >= capture_frames) FinishCapture();
    if (!trigger || recording.load() || writing.load()) return;
    std::lock_guard lock(mutex);
    events.clear(); bindings.clear(); dropped_events = 0; first_frame = frame;
    recording.store(true);
    Log("[RRStarFix] Recording 120 presents. Move the camera across the stars now.");
  } catch (...) { recording.store(false); Log("[RRStarFix] Present callback stopped after an internal error."); }
}
}
extern "C" __declspec(dllexport) const char *NAME = "RR Star Fix";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
  "Restores the stars that Ray Reconstruction removes from the night sky in The Witcher 3 (settings in RRStarFix.ini).";
extern "C" __declspec(dllexport) bool AddonInit(HMODULE module, HMODULE) {
  try {
  wchar_t path[32768];
  const DWORD length = GetModuleFileNameW(module, path, 32768);
  if (!length || length >= 32768) return false;
  ini = fs::path(path).parent_path() / "RRStarFix.ini";
  std::vector<std::string> warnings;
  settings = config::Load(ini, warnings);
  for (const auto &w : warnings) Log(("[RRStarFix] " + w).c_str());
  if (settings.diagnostics) {
    wchar_t local[32768];
    const DWORD local_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!local_length || local_length >= 32768) { settings.diagnostics = false; Log("[RRStarFix] LOCALAPPDATA unavailable; diagnostics off."); }
    else output = fs::path(local) / "RRStarFix" / "Captures";
  }
#ifndef RRSTARFIX_OFFLINE
  sl_probe::Start(output.parent_path() / "InputProbes", settings.diagnostics);
  star_composite::SetSmoothing(settings.smoothing);
#endif
  // The F10 trace's per-draw callbacks exist only with diagnostics on.
  if (settings.diagnostics) {
    reshade::register_event<reshade::addon_event::init_pipeline>(InitPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(DestroyPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(BindPipeline);
    reshade::register_event<reshade::addon_event::reset_command_list>(ResetCommand);
    reshade::register_event<reshade::addon_event::destroy_command_list>(ResetCommand);
    reshade::register_event<reshade::addon_event::draw>(Draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(DrawIndexed);
    reshade::register_event<reshade::addon_event::dispatch>(Dispatch);
  }
  reshade::register_event<reshade::addon_event::present>(Present);
  const std::string ready = std::string("[RRStarFix] v") + RRSTARFIX_VERSION + " ready. Star composite always on (after its safety check), " + config::SmoothingName(settings.smoothing) +
    " smoothing. " + config::KeyName(settings.smoothing_key) +
    " cycles smoothing." + (settings.diagnostics ? " Diagnostics ON: " + config::KeyName(settings.snapshot_key) +
    " snapshot, " + config::KeyName(settings.trace_key) + " trace." : "");
  Log(ready.c_str());
  return true;
  } catch (...) { return false; }
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE, HMODULE) {
#ifndef RRSTARFIX_OFFLINE
  sl_probe::Stop();
#endif
  recording.store(false);
  reshade::unregister_event<reshade::addon_event::present>(Present);
  if (settings.diagnostics) {
    reshade::unregister_event<reshade::addon_event::dispatch>(Dispatch);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(DrawIndexed);
    reshade::unregister_event<reshade::addon_event::draw>(Draw);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(ResetCommand);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(ResetCommand);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(BindPipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(DestroyPipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(InitPipeline);
  }
  if (writer.joinable()) writer.join();
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    return reshade::register_addon(module);
  }
  // AddonUninit owns callback and worker teardown outside the loader lock.
  if (reason == DLL_PROCESS_DETACH) reshade::unregister_addon(module);
  return TRUE;
}
