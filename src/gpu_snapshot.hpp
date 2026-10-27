#pragma once
#include <d3d12.h>
#include <sl.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
namespace gpu_snapshot {
struct Evaluation;
struct Counters {
  uint64_t allocations=0;   // readback buffers created
  uint64_t rejected=0;      // features abandoned after a validation/allocation failure
  uint64_t signaled=0;      // fences signalled on the presenting queue
  uint64_t write_attempts=0;
  uint64_t written=0;
  uint64_t pending=0;
  unsigned armed=0;
  unsigned captures=0;
};
void Start(const std::filesystem::path &directory);
void ObserveTags(unsigned viewport, const sl::ResourceTag *tags, unsigned count);
std::shared_ptr<Evaluation> Begin(unsigned feature, sl::CommandBuffer *command);
void End(const std::shared_ptr<Evaluation> &capture, sl::CommandBuffer *command, int result, bool composite);
void Present(ID3D12CommandQueue *queue);
void Stop();
Counters GetCounters();
// Arms one snapshot of the next RR/SR evaluation; returns a status line for the log.
std::string Arm();
void FailWritesForTesting(bool fail);
}
