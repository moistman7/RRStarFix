// Streamline API observations; every original call/argument is forwarded unchanged.
// Optional GPU snapshot copies (F9) and the automatic star composite run around slEvaluateFeature.
#include <windows.h>
#include <sl.h>
#include <MinHook.h>
#include "sl_probe.hpp"
#include "gpu_snapshot.hpp"
#include "star_composite.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace sl_probe {
namespace {
PFun_slSetTag *real_tag = nullptr;
PFun_slSetTagForFrame *real_frame_tag = nullptr;
PFun_slSetConstants *real_constants = nullptr;
PFun_slEvaluateFeature *real_evaluate = nullptr;
std::mutex gate;
std::ofstream file;
std::set<std::string> seen;
std::map<unsigned,unsigned> evaluations;
std::atomic<unsigned> active{0};
std::atomic<bool> enabled{false};
// Set once the 256-line log is full (or unavailable): later lines would be dropped by
// Write anyway, so callers skip formatting them.
std::atomic<bool> log_full{false};
bool attached = false, minhook_ready = false;
unsigned enabled_hooks = 0;
std::string status_message;
std::vector<void *> hooks;
struct Call { Call(){++active;} ~Call(){--active;} };

void Write(const std::string &line, bool unique = true) {
  std::lock_guard lock(gate);
  if (!file || seen.size() >= 256) { log_full.store(true); return; }
  if (unique && !seen.insert(line).second) return;
  file << GetTickCount64() << ' ' << line << '\n'; file.flush();
}
std::string Describe(unsigned viewport,const sl::ResourceTag *tags,unsigned count,bool command) {
  std::ostringstream s;s << "tags viewport=" << viewport << " command=" << command << " count=" << count;
  if (!tags && count) {s << " unavailable";return s.str();}
  // Only inspect the declared tag/resource structures. Never touch native GPU pointers.
  for(unsigned i=0;i<count && i<128;++i) {
    const auto &tag=tags[i];s << " | type=" << tag.type << " extent=" << tag.extent.width << 'x' << tag.extent.height;
    s << " resource=" << (tag.resource!=nullptr);
    if(tag.resource) {
      const auto &r=*tag.resource;
      s << " native=" << (r.native!=nullptr) << " size=" << r.width << 'x' << r.height
        << " format=" << r.nativeFormat << " state=" << r.state;
    }
  }
  return s.str();
}
sl::Result HookTag(const sl::ViewportHandle &v,const sl::ResourceTag *t,uint32_t n,sl::CommandBuffer *c) {
  Call call;
  try{if(enabled.load() && !log_full.load(std::memory_order_relaxed))Write(Describe(static_cast<unsigned>(v),t,n,c!=nullptr));}catch(...){}
  const auto result=real_tag(v,t,n,c);
  try{if(enabled.load() && result==sl::Result::eOk){gpu_snapshot::ObserveTags(static_cast<unsigned>(v),t,n);star_composite::ObserveTags(static_cast<unsigned>(v),t,n);}}catch(...){}
  return result;
}
sl::Result HookFrameTag(const sl::FrameToken &f,const sl::ViewportHandle &v,const sl::ResourceTag *t,uint32_t n,sl::CommandBuffer *c) {
  Call call;
  try{if(enabled.load() && !log_full.load(std::memory_order_relaxed))Write(Describe(static_cast<unsigned>(v),t,n,c!=nullptr));}catch(...){}
  const auto result=real_frame_tag(f,v,t,n,c);
  try{if(enabled.load() && result==sl::Result::eOk){gpu_snapshot::ObserveTags(static_cast<unsigned>(v),t,n);star_composite::ObserveTags(static_cast<unsigned>(v),t,n);}}catch(...){}
  return result;
}
sl::Result HookConstants(const sl::Constants &c,const sl::FrameToken &f,const sl::ViewportHandle &v) {
  Call call;
  try {
    if(enabled.load() && !log_full.load(std::memory_order_relaxed)) {
      std::ostringstream s;s << "constants viewport=" << static_cast<unsigned>(v)
        << " depthInverted=" << static_cast<int>(c.depthInverted)
        << " cameraMotionIncluded=" << static_cast<int>(c.cameraMotionIncluded)
        << " mvecScale=" << c.mvecScale.x << ',' << c.mvecScale.y
        << " invalidMotion=" << c.motionVectorsInvalidValue
        << " motionVectorsJittered=" << static_cast<int>(c.motionVectorsJittered)
        << " motionVectorsDilated=" << static_cast<int>(c.motionVectorsDilated)
        << " cameraNear=" << c.cameraNear << " cameraFar=" << c.cameraFar;
      Write(s.str());
    }
  }catch(...){}
  try{if(enabled.load())star_composite::ObserveConstants(static_cast<unsigned>(v),c.mvecScale.x,c.mvecScale.y);}catch(...){}
  return real_constants(c,f,v);
}
sl::Result HookEvaluate(sl::Feature feature,const sl::FrameToken &frame,const sl::BaseStructure **inputs,uint32_t count,sl::CommandBuffer *cmd) {
  Call call;
  std::shared_ptr<gpu_snapshot::Evaluation> pixels;
  try{if(enabled.load())pixels=gpu_snapshot::Begin(feature,cmd);}catch(...){}
  const auto result=real_evaluate(feature,frame,inputs,count,cmd);
  // The composite is recorded before the output copy so snapshots show what is displayed.
  bool composite=false;
  try{if(enabled.load())composite=star_composite::AfterEvaluate(feature,cmd,static_cast<int>(result));}catch(...){}
  try{gpu_snapshot::End(pixels,cmd,static_cast<int>(result),composite);}catch(...){}
  try {
    if(enabled.load()) {
      std::lock_guard lock(gate);
      auto &n=evaluations[feature];
      if(n<4 && file) {++n;file << GetTickCount64() << " evaluate feature=" << feature
        << " result=" << static_cast<int>(result) << " inputCount=" << count << " command=" << (cmd!=nullptr) << '\n';file.flush();}
    }
  }catch(...){}
  return result;
}
template<class T> void Hook(HMODULE module,const char *name,void *replacement,T **original) {
  auto *address=reinterpret_cast<void *>(GetProcAddress(module,name));
  if(!address){Write(std::string("missing export ")+name,false);return;}
  const auto status=MH_CreateHook(address,replacement,reinterpret_cast<void**>(original));
  if(status!=MH_OK){Write(std::string("hook unavailable ")+name+" status="+std::to_string(status),false);return;}
  hooks.push_back(address);
  const auto on=MH_EnableHook(address);
  if(on==MH_OK)++enabled_hooks;
  Write(std::string("hook ")+name+" status="+std::to_string(on),false);
}
}
void Start(const std::filesystem::path &directory,bool diagnostics) {
  std::lock_guard lock(gate);
  star_composite::Start();
  if(!diagnostics) {
    // No log file: Write() and the formatting in the hooks are skipped entirely.
    log_full.store(true);enabled.store(true);return;
  }
  gpu_snapshot::Start(directory.parent_path()/"PixelSnapshots");
  std::filesystem::create_directories(directory);
  log_full.store(false);
  file.open(directory/("streamline-inputs-"+std::to_string(GetTickCount64())+".log"));
  file << "RRStarFix 0.8 Streamline input metadata\n"
       << "Feature IDs: DLSS SR=0, frame generation=1000, RR=1001.\n"
       << "Buffer IDs: depth=0, motion=1, input color=3, output color=4, color-before-transparency=40.\n"
       << "GPU pixel snapshots, if taken, are under ../PixelSnapshots. Resource size/format fields may be zero when the caller leaves them unspecified.\n";
  file.flush();enabled.store(true);
}
void Poll() {
  if(attached || !enabled.load())return;
  HMODULE module=GetModuleHandleW(L"sl.interposer.dll");if(!module)return;
  attached=true;
  const auto status=MH_Initialize();
  if(status!=MH_OK){Write("MinHook initialization failed status="+std::to_string(status),false);
    std::lock_guard lock(gate);status_message="[RRStarFix] Streamline found, but hooking failed (MinHook "+std::to_string(status)+"); the star composite cannot run.";return;}
  minhook_ready=true;
  Hook(module,"slSetTag",reinterpret_cast<void*>(HookTag),&real_tag);
  Hook(module,"slSetTagForFrame",reinterpret_cast<void*>(HookFrameTag),&real_frame_tag);
  Hook(module,"slSetConstants",reinterpret_cast<void*>(HookConstants),&real_constants);
  Hook(module,"slEvaluateFeature",reinterpret_cast<void*>(HookEvaluate),&real_evaluate);
  std::lock_guard lock(gate);
  status_message=enabled_hooks==4?"[RRStarFix] Streamline hooks installed (4/4).":
    "[RRStarFix] Only "+std::to_string(enabled_hooks)+"/4 Streamline hooks installed; the star composite may not run.";
}
void Stop() {
  enabled.store(false);
  for(auto *address:hooks)MH_DisableHook(address);
  // Keep trampolines valid until any already-entered original invocation returns.
  while(active.load()!=0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  for(auto *address:hooks)MH_RemoveHook(address);
  // Composite detours are removed before MinHook is uninitialized.
  star_composite::Stop();
  hooks.clear();if(minhook_ready)MH_Uninitialize();
  gpu_snapshot::Stop();
  std::lock_guard lock(gate);if(file){file << "probe stopped\n";file.close();}
}
std::string TakeStatus() {
  std::lock_guard lock(gate);std::string out;out.swap(status_message);return out;
}
}
