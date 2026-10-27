#pragma once
#include <d3d12.h>
#include <sl.h>
#include <cstdint>
#include <string>
#include <vector>
namespace star_composite {
struct Stats {
  uint64_t applied=0;    // RR frames with the composite recorded
  uint64_t skipped=0;    // enabled RR frames skipped (tags, formats, states)
  uint64_t restored=0;   // RR frames where the game re-bound heaps, pipeline and root signature before use
  uint64_t closed=0;     // RR frames where the command list was closed/reset before any use
  uint64_t unsafe=0;     // RR frames where the game used bindings without re-binding them
  bool hooks=false,verified=false,enabled=false;
};
constexpr uint64_t required_safe_frames=3;
void Start();
void ObserveTags(unsigned viewport, const sl::ResourceTag *tags, unsigned count);
// Call after slEvaluateFeature returns on the same command list. Returns true
// when the composite was recorded. Always starts the state-restore check for RR.
bool AfterEvaluate(unsigned feature, sl::CommandBuffer *command, int result);
void ObserveConstants(unsigned viewport, float mvec_scale_x, float mvec_scale_y);
void Present();
// F7: cycles still-camera smoothing light/medium/strong (medium at launch).
std::string CycleSmoothing();
void SetSmoothing(unsigned level);   // 0 light, 1 medium, 2 strong
unsigned Smoothing();
// Log lines produced off the present thread (auto-disable, hook status).
std::vector<std::string> TakeMessages();
void Stop();
Stats GetStats();
void SetGainForTesting(float gain);
void SetAlphaForTesting(float alpha);
}
