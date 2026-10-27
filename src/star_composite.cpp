// Sky-only star composite after DLSS-RR, automatically enabled after its safety check.
// With Streamline's default eDisableCLStateTracking the game must re-bind state
// after slEvaluateFeature, because DLSS itself changes descriptor heaps, pipeline
// state and the compute root signature. The composite changes only those same
// bindings, so it can be enabled only after the game has been observed re-binding
// all of them before their next use on RR frames, and it switches itself off if
// the game is ever seen using them without re-binding.
#include "star_composite.hpp"
#include "star_composite_shader.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
using Microsoft::WRL::ComPtr;
namespace star_composite {
namespace {
struct Tag {
  ComPtr<ID3D12Resource> resource;
  uint32_t state=UINT_MAX;
  unsigned width=0,height=0,left=0,top=0;
};
struct Pipeline {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pso;
  ComPtr<ID3D12DescriptorHeap> heap;
  UINT increment=0;
  unsigned next=0;
  bool ok=false;
  std::map<DXGI_FORMAT,bool> typed_load;
  // Star history at output resolution: history[read] is in shader-resource
  // state and is read, the other is in UAV state and written; they swap per frame.
  ComPtr<ID3D12Resource> history[2];
  UINT64 width=0;UINT height=0;
  unsigned read=0;
};
constexpr unsigned ring_sets=64,set_size=6;
constexpr DXGI_FORMAT history_format=DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr UINT permitted=D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
  D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST |
  D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_DEPTH_WRITE;
std::mutex gate,message_gate;
// D3D12 references are heap-held and never destroyed, so nothing is released at
// process exit if ReShade skips AddonUninit (D3D12 may already be torn down).
std::map<unsigned,std::map<unsigned,Tag>> &views=*new std::map<unsigned,std::map<unsigned,Tag>>;
std::vector<Pipeline> &pipelines=*new std::vector<Pipeline>;
std::vector<std::string> messages;
std::set<std::string> said;
std::atomic<bool> running{false},enabled{false},hooks_ready{false},hooks_failed{false};
std::atomic<uint64_t> applied{0},skipped{0},restored{0},closed{0},unsafe{0};
std::atomic<float> gain{1.f},alpha_still{.03f},alpha_moving{.15f},mvec_x{1.f},mvec_y{1.f};
// F7 smoothing levels: per-frame blend weight while the camera is still.
constexpr float smoothing_levels[3]={.06f,.03f,.015f};
constexpr const char *smoothing_names[3]={"light","medium","strong"};
std::atomic<unsigned> smoothing{1};
std::atomic<bool> reset_history{true};
uint64_t presents=0;
// Replaced history buffers stay alive until in-flight frames have finished with them.
std::vector<std::pair<uint64_t,ComPtr<ID3D12Resource>>> &retired=*new std::vector<std::pair<uint64_t,ComPtr<ID3D12Resource>>>;
void Say(const std::string &message,bool once=true) {
  std::lock_guard lock(message_gate);
  if(messages.size()>=64 || (once && !said.insert(message).second))return;
  messages.push_back("[RRStarFix] "+message);
}

// ---- State-restore check -------------------------------------------------
// Bits a command list must re-bind after RR before using them again.
enum : unsigned { kHeaps=1, kPso=2, kRootCompute=4, kRootGraphics=8 };
struct Watch { bool active=false; void *list=nullptr; unsigned seen=0; };
thread_local Watch watch;
// Detour depth per thread: each recording thread only touches its own counter
// (one cache line each), so the hot path shares nothing between threads. Unhooking
// waits until every counter is 0. Counters are never freed, since a thread keeps
// its pointer for life; there is one per thread that ever entered a detour.
#pragma warning(push)
#pragma warning(disable:4324) // padding to a full cache line is the point
struct alignas(64) DetourDepth { std::atomic<int> depth{0}; };
#pragma warning(pop)
std::mutex depth_gate;
std::vector<DetourDepth*> depths;
thread_local DetourDepth *my_depth=nullptr;
DetourDepth &Depth() {
  if(!my_depth) {
    auto *d=new DetourDepth;
    {std::lock_guard lock(depth_gate);depths.push_back(d);}
    my_depth=d;
  }
  return *my_depth;
}
struct Enter { DetourDepth &d; Enter():d(Depth()){d.depth.fetch_add(1);} ~Enter(){d.depth.fetch_sub(1);} };
bool AnyInDetour() {
  std::lock_guard lock(depth_gate);
  for(auto *d:depths)if(d->depth.load()!=0)return true;
  return false;
}
void Finish(int outcome) {
  watch.active=false;
  if(outcome==0)++restored;else if(outcome==1)++closed;
  else {
    ++unsafe;
    if(enabled.exchange(false))Say("Star composite auto-disabled: the game used bindings after RR without re-binding them.",false);
  }
}
// The first command list touched on this thread after RR is the one being checked.
bool Mine(void *self) {
  if(!watch.active)return false;
  if(!watch.list)watch.list=self;
  return watch.list==self;
}
void Bind(void *self,unsigned bit){if(Mine(self))watch.seen|=bit;}
void Use(void *self,unsigned need,bool any_root=false) {
  if(!Mine(self))return;
  bool ok=(watch.seen&need)==need;
  if(any_root)ok=ok&&(watch.seen&(kRootCompute|kRootGraphics));
  Finish(ok?0:2);
}
void Closed(void *self){if(Mine(self))Finish(1);}
using List=ID3D12GraphicsCommandList;
using List4=ID3D12GraphicsCommandList4;
HRESULT(STDMETHODCALLTYPE *real_close)(List*);
HRESULT(STDMETHODCALLTYPE *real_reset)(List*,ID3D12CommandAllocator*,ID3D12PipelineState*);
void(STDMETHODCALLTYPE *real_draw)(List*,UINT,UINT,UINT,UINT);
void(STDMETHODCALLTYPE *real_draw_indexed)(List*,UINT,UINT,UINT,INT,UINT);
void(STDMETHODCALLTYPE *real_dispatch)(List*,UINT,UINT,UINT);
void(STDMETHODCALLTYPE *real_set_pso)(List*,ID3D12PipelineState*);
void(STDMETHODCALLTYPE *real_bundle)(List*,ID3D12GraphicsCommandList*);
void(STDMETHODCALLTYPE *real_heaps)(List*,UINT,ID3D12DescriptorHeap*const*);
void(STDMETHODCALLTYPE *real_root_compute)(List*,ID3D12RootSignature*);
void(STDMETHODCALLTYPE *real_root_graphics)(List*,ID3D12RootSignature*);
void(STDMETHODCALLTYPE *real_clear_uint)(List*,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const UINT*,UINT,const D3D12_RECT*);
void(STDMETHODCALLTYPE *real_clear_float)(List*,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const FLOAT*,UINT,const D3D12_RECT*);
void(STDMETHODCALLTYPE *real_indirect)(List*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64);
void(STDMETHODCALLTYPE *real_set_pso1)(List4*,ID3D12StateObject*);
void(STDMETHODCALLTYPE *real_rays)(List4*,const D3D12_DISPATCH_RAYS_DESC*);
void(STDMETHODCALLTYPE *real_mesh)(List4*,UINT,UINT,UINT);
HRESULT STDMETHODCALLTYPE HookClose(List *l){Enter e;Closed(l);return real_close(l);}
HRESULT STDMETHODCALLTYPE HookReset(List *l,ID3D12CommandAllocator *a,ID3D12PipelineState *p){Enter e;Closed(l);return real_reset(l,a,p);}
void STDMETHODCALLTYPE HookDraw(List *l,UINT a,UINT b,UINT c,UINT d){Enter e;Use(l,kHeaps|kPso|kRootGraphics);real_draw(l,a,b,c,d);}
void STDMETHODCALLTYPE HookDrawIndexed(List *l,UINT a,UINT b,UINT c,INT d,UINT f){Enter e;Use(l,kHeaps|kPso|kRootGraphics);real_draw_indexed(l,a,b,c,d,f);}
void STDMETHODCALLTYPE HookDispatch(List *l,UINT x,UINT y,UINT z){Enter e;Use(l,kHeaps|kPso|kRootCompute);real_dispatch(l,x,y,z);}
void STDMETHODCALLTYPE HookSetPso(List *l,ID3D12PipelineState *p){Enter e;Bind(l,kPso);real_set_pso(l,p);}
void STDMETHODCALLTYPE HookBundle(List *l,ID3D12GraphicsCommandList *b){Enter e;Use(l,kHeaps,true);real_bundle(l,b);}
void STDMETHODCALLTYPE HookHeaps(List *l,UINT n,ID3D12DescriptorHeap *const *h){Enter e;Bind(l,kHeaps);real_heaps(l,n,h);}
void STDMETHODCALLTYPE HookRootCompute(List *l,ID3D12RootSignature *r){Enter e;Bind(l,kRootCompute);real_root_compute(l,r);}
void STDMETHODCALLTYPE HookRootGraphics(List *l,ID3D12RootSignature *r){Enter e;Bind(l,kRootGraphics);real_root_graphics(l,r);}
void STDMETHODCALLTYPE HookClearUint(List *l,D3D12_GPU_DESCRIPTOR_HANDLE g,D3D12_CPU_DESCRIPTOR_HANDLE c,ID3D12Resource *r,const UINT *v,UINT n,const D3D12_RECT *rc){Enter e;Use(l,kHeaps);real_clear_uint(l,g,c,r,v,n,rc);}
void STDMETHODCALLTYPE HookClearFloat(List *l,D3D12_GPU_DESCRIPTOR_HANDLE g,D3D12_CPU_DESCRIPTOR_HANDLE c,ID3D12Resource *r,const FLOAT *v,UINT n,const D3D12_RECT *rc){Enter e;Use(l,kHeaps);real_clear_float(l,g,c,r,v,n,rc);}
void STDMETHODCALLTYPE HookIndirect(List *l,ID3D12CommandSignature *s,UINT m,ID3D12Resource *a,UINT64 ao,ID3D12Resource *c,UINT64 co){Enter e;Use(l,kHeaps|kPso,true);real_indirect(l,s,m,a,ao,c,co);}
void STDMETHODCALLTYPE HookSetPso1(List4 *l,ID3D12StateObject *s){Enter e;Bind(l,kPso);real_set_pso1(l,s);}
void STDMETHODCALLTYPE HookRays(List4 *l,const D3D12_DISPATCH_RAYS_DESC *d){Enter e;Use(l,kHeaps|kPso|kRootCompute);real_rays(l,d);}
void STDMETHODCALLTYPE HookMesh(List4 *l,UINT x,UINT y,UINT z){Enter e;Use(l,kHeaps|kPso|kRootGraphics);real_mesh(l,x,y,z);}
std::vector<void*> hooks;
// Indices verified against the Windows SDK d3d12.h C vtable (ID3D12GraphicsCommandList6Vtbl).
bool InstallHooks(ID3D12Device *device) {
  ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
  if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
     FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return false;
  list->Close();
  void **table=*reinterpret_cast<void***>(list.Get());
  struct Entry{unsigned index;void *detour;void **original;};
  std::vector<Entry> entries{
    {9,(void*)HookClose,(void**)&real_close},{10,(void*)HookReset,(void**)&real_reset},
    {12,(void*)HookDraw,(void**)&real_draw},{13,(void*)HookDrawIndexed,(void**)&real_draw_indexed},
    {14,(void*)HookDispatch,(void**)&real_dispatch},{25,(void*)HookSetPso,(void**)&real_set_pso},
    {27,(void*)HookBundle,(void**)&real_bundle},{28,(void*)HookHeaps,(void**)&real_heaps},
    {29,(void*)HookRootCompute,(void**)&real_root_compute},{30,(void*)HookRootGraphics,(void**)&real_root_graphics},
    {49,(void*)HookClearUint,(void**)&real_clear_uint},{50,(void*)HookClearFloat,(void**)&real_clear_float},
    {59,(void*)HookIndirect,(void**)&real_indirect}};
  ComPtr<ID3D12GraphicsCommandList4> list4;ComPtr<ID3D12GraphicsCommandList6> list6;
  if(SUCCEEDED(list.As(&list4))){entries.push_back({75,(void*)HookSetPso1,(void**)&real_set_pso1});entries.push_back({76,(void*)HookRays,(void**)&real_rays});}
  if(SUCCEEDED(list.As(&list6)))entries.push_back({79,(void*)HookMesh,(void**)&real_mesh});
  const auto init=MH_Initialize();
  if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
  for(const auto &entry:entries) {
    if(MH_CreateHook(table[entry.index],entry.detour,entry.original)!=MH_OK) {
      for(auto *h:hooks)MH_RemoveHook(h);hooks.clear();return false;
    }
    hooks.push_back(table[entry.index]);
  }
  for(auto *h:hooks)if(MH_EnableHook(h)!=MH_OK){for(auto *x:hooks){MH_DisableHook(x);}
    while(AnyInDetour())Sleep(0);for(auto *x:hooks)MH_RemoveHook(x);hooks.clear();return false;}
  return true;
}

// ---- Composite pass ------------------------------------------------------
Pipeline *GetPipeline(ID3D12Device *device) {
  for(auto &p:pipelines)if(p.device.Get()==device)return p.ok?&p:nullptr;
  if(pipelines.size()>=4)return nullptr;
  auto &p=pipelines.emplace_back();p.device=device;
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=4;
  ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=2;ranges[1].OffsetInDescriptorsFromTableStart=4;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants.Num32BitValues=12;
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={2,ranges};
  // A static sampler lives in the root signature, so no sampler heap is bound.
  D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC desc{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob,error;
  if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)) ||
     FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p.root))))return nullptr;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=p.root.Get();
  pso.CS={star_composite_shader,sizeof(star_composite_shader)};
  if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&p.pso))))return nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors=ring_sets*set_size;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&p.heap))))return nullptr;
  p.increment=device->GetDescriptorHandleIncrementSize(heap.Type);p.ok=true;
  return &p;
}
DXGI_FORMAT ColorView(DXGI_FORMAT f) {
  switch(f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
DXGI_FORMAT MotionView(DXGI_FORMAT f) {
  switch(f) {
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
// (Re)creates the two history buffers for an output size; old ones are retired, not freed.
bool EnsureHistory(Pipeline &p,UINT64 width,UINT height) {
  if(p.history[0] && p.width==width && p.height==height)return true;
  for(auto &h:p.history)if(h)retired.emplace_back(presents,std::move(h));
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
  desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=history_format;desc.SampleDesc.Count=1;
  desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  for(int i=0;i<2;++i)
    if(FAILED(p.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
       i?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&p.history[i])))) {
      p.history[0].Reset();p.history[1].Reset();return false;
    }
  p.width=width;p.height=height;p.read=0;reset_history=true;
  return true;
}
DXGI_FORMAT DepthView(DXGI_FORMAT f) {
  switch(f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
bool Plain2D(const D3D12_RESOURCE_DESC &d,const Tag &t) {
  if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize!=1 || d.SampleDesc.Count!=1)return false;
  if(t.left || t.top)return false;
  return (!t.width || t.width==d.Width) && (!t.height || t.height==d.Height);
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r,UINT from,UINT to) {
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,static_cast<D3D12_RESOURCE_STATES>(from),static_cast<D3D12_RESOURCE_STATES>(to)};
  return b;
}
// Plans a shader read in the declared state, transitioning only when needed.
bool PlanRead(const Tag &t,std::vector<D3D12_RESOURCE_BARRIER> &before,std::vector<D3D12_RESOURCE_BARRIER> &after) {
  if(t.state & ~permitted)return false;
  if(t.state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)return true;
  before.push_back(Transition(t.resource.Get(),t.state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
  after.push_back(Transition(t.resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,t.state));
  return true;
}
bool Record(ID3D12GraphicsCommandList *cmd,std::map<unsigned,Tag> &tags) {
  if(cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT){Say("Star composite skipped: RR is not on a direct command list.");return false;}
  ComPtr<ID3D12Device> device;if(FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))return false;
  auto *p=GetPipeline(device.Get());
  if(!p){Say("Star composite unavailable: could not create its pipeline.");return false;}
  const auto &color=tags.at(3),&depth=tags.at(0),&motion=tags.at(1),&out=tags.at(4);
  const auto cd=color.resource->GetDesc(),dd=depth.resource->GetDesc(),md=motion.resource->GetDesc(),od=out.resource->GetDesc();
  if(!Plain2D(cd,color) || !Plain2D(dd,depth) || !Plain2D(md,motion) || !Plain2D(od,out) ||
     cd.Width!=dd.Width || cd.Height!=dd.Height || cd.Width!=md.Width || cd.Height!=md.Height){
    Say("Star composite skipped: unexpected buffer layout.");return false;}
  const auto cf=ColorView(cd.Format),df=DepthView(dd.Format),mf=MotionView(md.Format),of=ColorView(od.Format);
  if(cf==DXGI_FORMAT_UNKNOWN || df==DXGI_FORMAT_UNKNOWN || mf==DXGI_FORMAT_UNKNOWN || of==DXGI_FORMAT_UNKNOWN ||
     (cd.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || (dd.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
     (md.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || !(od.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)){
    Say("Star composite skipped: unsupported buffer formats "+std::to_string(cd.Format)+"/"+std::to_string(dd.Format)+"/"+
      std::to_string(md.Format)+"/"+std::to_string(od.Format)+".");return false;}
  auto loaded=p->typed_load.find(of);
  if(loaded==p->typed_load.end()) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{of};
    const bool ok=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) &&
      (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    loaded=p->typed_load.emplace(of,ok).first;
  }
  if(!loaded->second){Say("Star composite unavailable: GPU cannot read/write the output format in place.");return false;}
  std::vector<D3D12_RESOURCE_BARRIER> before,after;
  if(!PlanRead(color,before,after) || !PlanRead(depth,before,after) || !PlanRead(motion,before,after) || (out.state & ~permitted)){
    Say("Star composite skipped a frame: unsupported declared state.");return false;}
  if(!EnsureHistory(*p,od.Width,od.Height)){Say("Star composite unavailable: could not create its history buffers.");return false;}
  if(out.state!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    before.push_back(Transition(out.resource.Get(),out.state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    after.push_back(Transition(out.resource.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,out.state));
  }
  auto *read=p->history[p->read].Get(),*write=p->history[1-p->read].Get();
  // The written history becomes next frame's input; the read one becomes next frame's target.
  after.push_back(Transition(write,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
  after.push_back(Transition(read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
  // Each frame uses its own descriptor set; 64 frames is far beyond GPU latency.
  const unsigned slot=(p->next++%ring_sets)*set_size;
  auto cpu=p->heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(slot)*p->increment;
  auto gpu=p->heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(slot)*p->increment;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
  srv.Format=cf;device->CreateShaderResourceView(color.resource.Get(),&srv,cpu);cpu.ptr+=p->increment;
  srv.Format=df;device->CreateShaderResourceView(depth.resource.Get(),&srv,cpu);cpu.ptr+=p->increment;
  srv.Format=mf;device->CreateShaderResourceView(motion.resource.Get(),&srv,cpu);cpu.ptr+=p->increment;
  srv.Format=history_format;device->CreateShaderResourceView(read,&srv,cpu);cpu.ptr+=p->increment;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=of;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(out.resource.Get(),nullptr,&uav,cpu);cpu.ptr+=p->increment;
  uav.Format=history_format;device->CreateUnorderedAccessView(write,nullptr,&uav,cpu);
  if(!before.empty())cmd->ResourceBarrier(UINT(before.size()),before.data());
  D3D12_RESOURCE_BARRIER written{};written.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;written.UAV.pResource=out.resource.Get();
  cmd->ResourceBarrier(1,&written);
  ID3D12DescriptorHeap *heaps[]={p->heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
  cmd->SetComputeRootSignature(p->root.Get());cmd->SetPipelineState(p->pso.Get());
  const float f[6]={float(cd.Width)/float(od.Width),float(cd.Height)/float(od.Height),mvec_x.load(),mvec_y.load(),alpha_still.load(),gain.load()};
  UINT constants[12]={UINT(cd.Width),UINT(cd.Height),UINT(od.Width),od.Height};
  std::memcpy(&constants[4],f,sizeof(f));constants[10]=reset_history.exchange(false)?1u:0u;
  const float moving=alpha_moving.load();std::memcpy(&constants[11],&moving,4);
  cmd->SetComputeRoot32BitConstants(0,12,constants,0);cmd->SetComputeRootDescriptorTable(1,gpu);
  cmd->Dispatch(UINT((od.Width+7)/8),(od.Height+7)/8,1);
  cmd->ResourceBarrier(1,&written);
  cmd->ResourceBarrier(UINT(after.size()),after.data());
  p->read=1-p->read;
  return true;
}
}
// The game has been seen re-binding state after RR often enough, and never failing to.
bool Verified() {
  return hooks_ready.load() && unsafe.load()==0 && restored.load()+closed.load()>=required_safe_frames;
}
void Start() {
  std::lock_guard lock(gate);views.clear();enabled=false;
  applied=0;skipped=0;restored=0;closed=0;unsafe=0;running.store(true);
}
void ObserveTags(unsigned viewport,const sl::ResourceTag *tags,unsigned count) {
  if(!running.load() || !tags || count>128)return;
  // Tags are needed only to install the state check once, and to draw while on or about to turn on.
  if(!enabled.load() && unsafe.load() && (hooks_ready.load() || hooks_failed.load()))return;
  std::lock_guard lock(gate);
  if(views.size()>=4 && !views.contains(viewport))return;
  for(unsigned i=0;i<count;++i) {
    const auto &tag=tags[i];
    if(tag.type!=0 && tag.type!=1 && tag.type!=3 && tag.type!=4)continue;
    if(!tag.resource || !tag.resource->native || tag.lifecycle==sl::ResourceLifecycle::eOnlyValidNow ||
       tag.resource->type!=sl::ResourceType::eTex2d){views[viewport].erase(tag.type);continue;}
    Tag value;value.state=tag.resource->state;value.width=tag.extent.width;value.height=tag.extent.height;
    value.left=tag.extent.left;value.top=tag.extent.top;
    if(SUCCEEDED(static_cast<IUnknown*>(tag.resource->native)->QueryInterface(IID_PPV_ARGS(&value.resource))))
      views[viewport][tag.type]=std::move(value);
  }
}
bool AfterEvaluate(unsigned feature,sl::CommandBuffer *native,int result) {
  watch.active=false;
  if(!running.load() || feature!=sl::kFeatureDLSS_RR || !native)return false;
  bool recorded=false;
  try {
    std::lock_guard lock(gate);
    const bool tagged=views.size()==1 && views.begin()->second.contains(0) && views.begin()->second.contains(1) &&
      views.begin()->second.contains(3) && views.begin()->second.contains(4);
    if(!hooks_ready.load() && !hooks_failed.load() && tagged) {
      ComPtr<ID3D12Device> device;
      const bool ok=SUCCEEDED(views.begin()->second.at(0).resource->GetDevice(IID_PPV_ARGS(&device))) && InstallHooks(device.Get());
      (ok?hooks_ready:hooks_failed).store(true);
      Say(ok?"State-restore check installed; the star composite can turn on after 3 verified RR frames.":
             "State-restore check could not be installed; the star composite stays off.");
    }
    // Enable automatically as soon as the safety check passes.
    if(!enabled.load() && Verified()) {
      reset_history=true;enabled.store(true);
      Say("Star composite ON (state check passed: "+std::to_string(restored.load())+" re-bound, "+
        std::to_string(closed.load())+" closed, 0 unsafe).",false);
    }
    if(enabled.load()) {
      ComPtr<ID3D12GraphicsCommandList> cmd;
      if(result==0 && tagged && SUCCEEDED(static_cast<IUnknown*>(native)->QueryInterface(IID_PPV_ARGS(&cmd))))
        recorded=Record(cmd.Get(),views.begin()->second);
      ++(recorded?applied:skipped);
    }
  }catch(...){}
  // Checked after our own commands, so only the game's calls are counted.
  if(hooks_ready.load())watch=Watch{true,nullptr,0};
  return recorded;
}
void Present() {
  if(!running.load())return;
  std::lock_guard lock(gate);views.clear();++presents;
  // 120 presents is far beyond any GPU latency for a replaced history buffer.
  std::erase_if(retired,[](const auto &r){return presents-r.first>120;});
}
std::vector<std::string> TakeMessages() {
  std::lock_guard lock(message_gate);std::vector<std::string> out;out.swap(messages);return out;
}
void Stop() {
  running.store(false);enabled.store(false);
  for(auto *h:hooks)MH_DisableHook(h);
  // Keep trampolines valid until every detour already entered has returned.
  while(AnyInDetour())Sleep(0);
  for(auto *h:hooks)MH_RemoveHook(h);
  hooks.clear();hooks_ready=false;hooks_failed=false;
  std::lock_guard lock(gate);views.clear();
  // Command lists in flight may still reference the pipeline and descriptor heap.
  static auto *kept=new std::vector<Pipeline>;
  for(auto &p:pipelines)kept->push_back(std::move(p));
  static auto *kept_history=new std::vector<std::pair<uint64_t,ComPtr<ID3D12Resource>>>;
  for(auto &r:retired)kept_history->push_back(std::move(r));
  retired.clear();
  pipelines.clear();
}
Stats GetStats() {
  Stats s;s.applied=applied.load();s.skipped=skipped.load();s.restored=restored.load();s.closed=closed.load();
  s.unsafe=unsafe.load();s.hooks=hooks_ready.load();s.enabled=enabled.load();
  s.verified=Verified();
  return s;
}
void ObserveConstants(unsigned,float mvec_scale_x,float mvec_scale_y) {
  if(std::isfinite(mvec_scale_x) && std::isfinite(mvec_scale_y)){mvec_x.store(mvec_scale_x);mvec_y.store(mvec_scale_y);}
}
void SetGainForTesting(float value){gain.store(value);}
std::string CycleSmoothing() {
  const unsigned level=(smoothing.load()+1)%3;smoothing.store(level);alpha_still.store(smoothing_levels[level]);
  return "[RRStarFix] Star smoothing "+std::string(smoothing_names[level])+" (still-camera blend "+
    std::to_string(smoothing_levels[level]).substr(0,5)+" per frame).";
}
void SetSmoothing(unsigned level) {
  if(level>2)level=1;smoothing.store(level);alpha_still.store(smoothing_levels[level]);
}
unsigned Smoothing(){return smoothing.load();}
void SetAlphaForTesting(float value){alpha_still.store(value);alpha_moving.store(value);}
}
