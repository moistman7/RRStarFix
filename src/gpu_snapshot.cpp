// Hotkey-armed, bounded input/output snapshots of RR or SR evaluations. Copies use the
// resource states declared by the caller to Streamline and restore those states.
// Only tags set since the last present are used; game resource references are
// released at every present. Readback memory is never released before its fence
// completes, and files are written on a worker thread, once, success or failure.
#include "gpu_snapshot.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace gpu_snapshot {
namespace {
struct Tag {
  ComPtr<ID3D12Resource> resource;
  uint32_t state=UINT_MAX;
  unsigned width=0,height=0,left=0,top=0;
};
struct Image {
  unsigned type=0;
  Tag source;
  ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 size=0;
};
std::mutex gate;
std::mutex log_gate;
fs::path output;
// Objects holding D3D12 references or threads are heap-held and never destroyed:
// when ReShade skips AddonUninit their destructors would run at process exit, where
// an unjoined thread aborts the process and D3D12 may already be torn down.
std::map<unsigned,std::map<unsigned,Tag>> &views=*new std::map<unsigned,std::map<unsigned,Tag>>;
std::set<unsigned> abandoned_features;
std::atomic<unsigned> armed{0};
unsigned captures=0;
constexpr unsigned max_captures=8;
std::atomic<bool> running{false};
std::atomic<bool> fail_writes{false};
std::atomic<uint64_t> failed{0},allocations{0},rejected{0},signaled{0},write_attempts{0},written{0};
std::ofstream status_log;
std::set<std::string> status_seen;
std::vector<std::thread> &writers=*new std::vector<std::thread>;
constexpr UINT64 memory_limit=256ull*1024*1024;
void Status(const std::string &message) {
  std::lock_guard lock(log_gate);
  if(status_seen.size()<64 && status_seen.insert(message).second && status_log) {
    status_log << GetTickCount64() << ' ' << message << '\n';status_log.flush();
  }
}
void Check(HRESULT result){if(FAILED(result))throw result;}
// Checks a tag and computes its footprint without allocating anything.
bool Validate(ID3D12Device *device,unsigned type,const Tag &tag,Image &image) {
  if(!tag.resource || tag.state==UINT_MAX)return false;
  const auto desc=tag.resource->GetDesc();
  if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 ||
     desc.MipLevels!=1 || desc.SampleDesc.Count!=1 || desc.Width>16384 || desc.Height>16384)return false;
  constexpr unsigned permitted=D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST |
    D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_DEPTH_WRITE;
  if(tag.state & ~permitted)return false;
  image.type=type;image.source=tag;
  device->GetCopyableFootprints(&desc,0,1,0,&image.footprint,nullptr,nullptr,&image.size);
  return image.size && image.size<=memory_limit;
}
bool Allocate(ID3D12Device *device,Image &image) {
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width=image.size;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
  buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const bool ok=SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
    D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image.readback)));
  if(ok)++allocations;
  return ok;
}
void Copy(ID3D12GraphicsCommandList *command,const Image &image) {
  const auto original=static_cast<D3D12_RESOURCE_STATES>(image.source.state);
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition={image.source.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,original,D3D12_RESOURCE_STATE_COPY_SOURCE};
  if(original!=D3D12_RESOURCE_STATE_COPY_SOURCE)command->ResourceBarrier(1,&barrier);
  D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=image.source.resource.Get();
  source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=image.readback.Get();
  dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dest.PlacedFootprint=image.footprint;
  command->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
  if(original!=D3D12_RESOURCE_STATE_COPY_SOURCE) {
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter=original;command->ResourceBarrier(1,&barrier);
  }
}
}
struct Evaluation {
  unsigned feature=0;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12Fence> fence;
  std::vector<Image> images;
  size_t input_count=0;
  bool ready=false,output_recorded=false,signaled=false,composite=false;
  unsigned index=0;
  unsigned presents_after_ready=0;
  int result=0;
};
namespace {
std::vector<std::shared_ptr<Evaluation>> &pending=*new std::vector<std::shared_ptr<Evaluation>>;
// Captures whose GPU copies may still be in flight are kept alive deliberately.
std::vector<std::shared_ptr<Evaluation>> &Abandoned() {
  static auto *abandoned=new std::vector<std::shared_ptr<Evaluation>>;return *abandoned;
}
void Write(const Evaluation &capture) {
  if(fail_writes.load())throw std::runtime_error("Injected write failure");
  const auto folder=output/((capture.feature==sl::kFeatureDLSS_RR?"RR-":"SR-")+std::to_string(capture.index)+"-"+std::to_string(GetTickCount64()));
  fs::create_directories(folder);
  std::ofstream manifest(folder/"manifest.csv");
  manifest << "buffer_type,phase,width,height,format,row_pitch,bytes,extent_left,extent_top,extent_width,extent_height,declared_state\n";
  const size_t count=capture.output_recorded?capture.images.size():capture.input_count;
  for(size_t i=0;i<count;++i) {
    const auto &image=capture.images[i];
    const char *phase=i<capture.input_count?"before":"after";
    const auto name=std::to_string(image.type)+"-"+phase+".raw";
    void *data=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(image.size)};
    Check(image.readback->Map(0,&range,&data));
    try {
      std::ofstream file(folder/name,std::ios::binary);
      file.write(static_cast<const char*>(data),static_cast<std::streamsize>(image.size));
      if(!file)throw std::runtime_error("Pixel write failed");
    }catch(...){D3D12_RANGE empty{0,0};image.readback->Unmap(0,&empty);throw;}
    D3D12_RANGE empty{0,0};image.readback->Unmap(0,&empty);
    const auto &fp=image.footprint.Footprint;
    manifest << image.type << ',' << phase << ',' << fp.Width << ',' << fp.Height << ','
      << static_cast<unsigned>(fp.Format) << ',' << fp.RowPitch << ',' << image.size << ','
      << image.source.left << ',' << image.source.top << ',' << image.source.width << ','
      << image.source.height << ',' << image.source.state << '\n';
  }
  std::ofstream status(folder/"status.txt");
  status << "feature=" << capture.feature << "\nStreamline_result=" << capture.result
    << "\nstar_composite_applied=" << capture.composite
    << "\nCopies taken immediately before and after feature evaluation.\n"
    << (capture.output_recorded?"":"Output copy could not be recorded; inputs only.\n")
    << "The other mode's snapshot is a separate frame, not pixel-aligned to this frame.\n";
  if(!manifest || !status)throw std::runtime_error("Manifest write failed");
}
// Exactly one attempt per capture; the capture is released whatever the outcome.
void WriteOnce(std::shared_ptr<Evaluation> capture) {
  ++write_attempts;
  try {
    Write(*capture);++written;
    Status("Saved GPU pixels for feature="+std::to_string(capture->feature));
  }catch(...){++failed;Status("Pixel write failed for feature="+std::to_string(capture->feature)+"; snapshot discarded, not retried.");}
}
}
void Start(const fs::path &directory) {
  std::lock_guard lock(gate);output=directory/("run-"+std::to_string(GetTickCount64()));
  fs::create_directories(output);
  {std::lock_guard log(log_gate);status_log.open(output/"pixel-probe.log");status_seen.clear();}
  Status("Pixel probe initialized; press F9 to capture the next RR or SR evaluation (max 8).");
  allocations=0;rejected=0;signaled=0;write_attempts=0;written=0;failed=0;
  abandoned_features.clear();armed=0;captures=0;views.clear();pending.clear();running.store(true);
}
void ObserveTags(unsigned viewport,const sl::ResourceTag *tags,unsigned count) {
  // Unarmed (the normal case): return before taking the lock; re-checked under it.
  if(!running.load() || !tags || count>128 || armed.load()==0)return;
  std::lock_guard lock(gate);
  // Game resources are referenced only while a snapshot is armed.
  if(armed.load()==0 || (views.size()>=4 && !views.contains(viewport)))return;
  for(unsigned i=0;i<count;++i) {
    const auto &tag=tags[i];
    if(tag.type!=0 && tag.type!=1 && tag.type!=3 && tag.type!=4 && tag.type!=40)continue;
    if(!tag.resource || !tag.resource->native) {views[viewport].erase(tag.type);continue;}
    // Declared state is only guaranteed at tag time for this lifecycle.
    if(tag.lifecycle==sl::ResourceLifecycle::eOnlyValidNow) {
      views[viewport].erase(tag.type);Status("Ignored eOnlyValidNow tag type="+std::to_string(tag.type));continue;
    }
    if(tag.resource->type!=sl::ResourceType::eTex2d)continue;
    Tag value;value.state=tag.resource->state;value.width=tag.extent.width;value.height=tag.extent.height;
    value.left=tag.extent.left;value.top=tag.extent.top;
    auto *unknown=static_cast<IUnknown*>(tag.resource->native);
    if(SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&value.resource))))views[viewport][tag.type]=std::move(value);
  }
}
std::shared_ptr<Evaluation> Begin(unsigned feature,sl::CommandBuffer *native) {
  if(!running.load() || (feature!=sl::kFeatureDLSS_RR && feature!=sl::kFeatureDLSS) || !native || armed.load()==0)return {};
  try {
    std::lock_guard lock(gate);
    if(armed.load()==0 || abandoned_features.contains(feature) || views.size()!=1)return {};
    auto &tags=views.begin()->second;
    // Missing tags are a cheap skip and may be retried next frame.
    if(!tags.contains(0) || !tags.contains(1) || !tags.contains(3) || !tags.contains(4))return {};
    // Any later failure abandons this feature so nothing is reallocated every frame.
    auto reject=[&](const std::string &why){abandoned_features.insert(feature);++rejected;Status(why);return std::shared_ptr<Evaluation>{};};
    ComPtr<ID3D12GraphicsCommandList> command;
    if(FAILED(static_cast<IUnknown*>(native)->QueryInterface(IID_PPV_ARGS(&command))))return reject("Skipped: command buffer is not a D3D12 graphics command list.");
    // Fence completion is associated with the direct presentation queue only.
    if(command->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return reject("Skipped: evaluation is not on a direct command list.");
    auto capture=std::make_shared<Evaluation>();capture->feature=feature;
    // Streamline and ReShade wrap the device, so pointers differ between layers;
    // the adapter LUID identifies the GPU. The fence is created at present time.
    if(FAILED(tags.at(0).resource->GetDevice(IID_PPV_ARGS(&capture->device))))return reject("Skipped: resource device unavailable.");
    std::vector<unsigned> types{0,1,3};if(feature==sl::kFeatureDLSS_RR && tags.contains(40))types.push_back(40);
    types.push_back(4);
    UINT64 total=0;
    // Validation allocates nothing, so a frame with an odd declared state is
    // skipped and retried; the game has been seen declaring 0x80000000 for depth.
    auto skip=[&](const std::string &why){Status(why);return std::shared_ptr<Evaluation>{};};
    for(unsigned type:types) {
      Image image;
      if(!Validate(capture->device.Get(),type,tags.at(type),image))
        return skip("Skipped frame: unsupported format/state for type="+std::to_string(type)+
          " state="+std::to_string(tags.at(type).state)+" feature="+std::to_string(feature));
      total+=image.size;
      if(total>memory_limit)return skip("Skipped frame: snapshot exceeds 256 MiB; feature="+std::to_string(feature));
      capture->images.push_back(std::move(image));
    }
    capture->input_count=capture->images.size()-1;
    for(auto &image:capture->images)
      if(!Allocate(capture->device.Get(),image))return reject("Readback allocation failed; feature="+std::to_string(feature)+" abandoned.");
    --armed;capture->index=++captures;pending.push_back(capture);
    // No GPU commands are inserted until allocation and format checks all succeed.
    for(size_t i=0;i<capture->input_count;++i)Copy(command.Get(),capture->images[i]);
    Status("Recorded input copies for feature="+std::to_string(feature));return capture;
  }catch(...){++failed;return {};}
}
void End(const std::shared_ptr<Evaluation> &capture,sl::CommandBuffer *native,int result,bool composite) {
  if(!capture)return;
  std::lock_guard lock(gate);
  // Inputs are already recorded, so the capture must still be fenced before release.
  ComPtr<ID3D12GraphicsCommandList> command;
  if(native && SUCCEEDED(static_cast<IUnknown*>(native)->QueryInterface(IID_PPV_ARGS(&command)))) {
    Copy(command.Get(),capture->images.back());capture->output_recorded=true;
  } else Status("Output copy could not be recorded; inputs only.");
  capture->result=result;capture->composite=composite;capture->ready=true;
}
void Present(ID3D12CommandQueue *queue) {
  if(!queue || !running.load())return;
  try {
    std::lock_guard lock(gate);
    // Tags must be set again each frame; this also releases game resource references.
    views.clear();
    // Nothing in flight (the normal case): no device query needed.
    if(pending.empty())return;
    ComPtr<ID3D12Device> device;Check(queue->GetDevice(IID_PPV_ARGS(&device)));
    for(auto it=pending.begin();it!=pending.end();) {
      auto &capture=**it;
      if(!capture.ready){++it;continue;}
      const auto mine=capture.device->GetAdapterLuid(),theirs=device->GetAdapterLuid();
      if(mine.LowPart!=theirs.LowPart || mine.HighPart!=theirs.HighPart) {
        Status("Presenting queue is on a different adapter (snapshot LUID "+std::to_string(mine.HighPart)+":"+
          std::to_string(mine.LowPart)+", queue LUID "+std::to_string(theirs.HighPart)+":"+
          std::to_string(theirs.LowPart)+"); snapshot kept unread.");
        Abandoned().push_back(*it);it=pending.erase(it);continue;
      }
      if(!capture.signaled) {
        // Wait one extra present so the evaluating command list has been submitted.
        if(++capture.presents_after_ready<2){++it;continue;}
        // A fence from the queue's own device is always valid for that queue.
        if(!capture.fence && FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&capture.fence)))) {
          Status("Fence creation failed; snapshot kept unread.");Abandoned().push_back(*it);it=pending.erase(it);continue;
        }
        Check(queue->Signal(capture.fence.Get(),1));capture.signaled=true;++signaled;
      }
      const auto completed=capture.fence->GetCompletedValue();
      if(completed==UINT64_MAX){Status("Device removed; pixel snapshot discarded.");Abandoned().push_back(*it);it=pending.erase(it);continue;}
      if(completed<1){++it;continue;}
      writers.emplace_back(WriteOnce,*it);it=pending.erase(it);
    }
  }catch(...){++failed;}
}
void Stop() {
  running.store(false);
  std::vector<std::thread> joining;
  {
    std::lock_guard lock(gate);
    // Normal game shutdown/unload occurs after presentation; never map an unfinished copy.
    for(const auto &capture:pending) {
      if(capture->signaled && capture->fence && capture->fence->GetCompletedValue()>=1 &&
         capture->fence->GetCompletedValue()!=UINT64_MAX)WriteOnce(capture);
      else Abandoned().push_back(capture);
    }
    pending.clear();views.clear();joining.swap(writers);
  }
  for(auto &writer:joining)if(writer.joinable())writer.join();
  Status("Pixel probe stopped.");
  std::lock_guard log(log_gate);status_log.close();
}
Counters GetCounters() {
  std::lock_guard lock(gate);
  return {allocations.load(),rejected.load(),signaled.load(),write_attempts.load(),written.load(),pending.size(),armed.load(),captures};
}
std::string Arm() {
  std::lock_guard lock(gate);
  if(!running.load())return "Pixel snapshot unavailable.";
  if(captures+armed.load()>=max_captures)return "Pixel snapshot limit (8 per session) reached.";
  if(armed.load())return "Pixel snapshot already armed.";
  armed=1;Status("Snapshot armed.");
  return "Pixel snapshot armed for the next RR/SR evaluation ("+std::to_string(captures+1)+"/8).";
}
void FailWritesForTesting(bool fail){fail_writes.store(fail);}
}
