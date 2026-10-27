// Star composite on Windows' software D3D12 adapter: the state-restore check
// (installed detours, automatic activation and safety disable) and the actual
// compiled shader adding a sky star into RR's output while leaving non-sky,
// sub-threshold and background pixels unchanged.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "star_composite.hpp"
#include "star_composite_shader.hpp"
using Microsoft::WRL::ComPtr;
void HR(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed: " + std::to_string(hr)); }
void Expect(bool condition,const std::string &what){if(!condition)throw std::runtime_error(what);}
int main(int argc,char **argv) {
  // --exit-without-stop: leave with detours installed and composite buffers alive, as
  // when ReShade skips AddonUninit. Must exit cleanly (code 0).
  const bool exit_without_stop=argc==2 && std::string(argv[1])=="--exit-without-stop";
  if(exit_without_stop){_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);}
  try {
    ComPtr<ID3D12Debug> debug;
    const bool validated=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if(validated)debug->EnableDebugLayer();
    // RRSTARFIX_GBV=1 also validates resource states while commands run on the GPU.
    if(char *gbv=nullptr;validated && _dupenv_s(&gbv,nullptr,"RRSTARFIX_GBV")==0 && gbv){const bool on=gbv[0]=='1';free(gbv);
      ComPtr<ID3D12Debug1> d1;if(on && SUCCEEDED(debug.As(&d1))){d1->SetEnableGPUBasedValidation(TRUE);std::cout << "GPU-based validation on.\n";}}
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> d; HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
    // Any debug-layer error (bad barrier state, descriptor, binding) fails the test.
    ComPtr<ID3D12InfoQueue> info;if(validated)d.As(&info);
    auto api_errors=[&]{
      std::string text;if(!info)return text;
      for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);
        std::vector<char> m(n);auto *msg=reinterpret_cast<D3D12_MESSAGE*>(m.data());info->GetMessage(i,msg,&n);
        if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)text+=std::string(msg->pDescription)+"\n";}
      return text;
    };
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R32G32B32A32_FLOAT};
    HR(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)));
    if(!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)){std::cout<<"SKIP: no typed UAV load on WARP\n";return 77;}
    Expect(MH_Initialize()==MH_OK,"MinHook init failed");
    constexpr unsigned W=40,H=20,OW=80,OH=40;
    auto texture=[&](DXGI_FORMAT format,unsigned w,unsigned h,bool uav) {
      D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
      desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
      if(uav)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      ComPtr<ID3D12Resource> r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
      D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width=size;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ComPtr<ID3D12Resource> r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    // RR input color is tagged in UNORDERED_ACCESS (as in the game), depth and motion in shader-resource state.
    auto color=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,true);
    auto depth=texture(DXGI_FORMAT_R32_TYPELESS,W,H,false);
    auto motion=texture(DXGI_FORMAT_R32G32_FLOAT,W,H,false);
    auto output=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,OW,OH,true);
    const UINT color_pitch=1024,depth_pitch=256,out_pitch=1280,motion_pitch=512;
    // Upload regions (512-byte aligned): star color, depth, RR output, starless color, still and shifted motion.
    const size_t star_at=0,depth_at=color_pitch*H,out_at=depth_at+depth_pitch*H,empty_at=out_at+out_pitch*OH,
      still_at=empty_at+color_pitch*H,shift_at=still_at+motion_pitch*H,upload_size=shift_at+motion_pitch*H;
    auto upload=buffer(upload_size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback=buffer(out_pitch*OH,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    unsigned char *data=nullptr;HR(upload->Map(0,nullptr,reinterpret_cast<void**>(&data)));std::memset(data,0,upload_size);
    auto put=[&](size_t offset,UINT pitch,unsigned x,unsigned y,float v){float p[4]={v,v,v,1};std::memcpy(data+offset+y*pitch+x*16,p,16);};
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x) {
      put(star_at,color_pitch,x,y,.01f);put(empty_at,color_pitch,x,y,.01f);
      const float z=x>=28?.5f:0.f;std::memcpy(data+depth_at+y*depth_pitch+x*4,&z,4);
      // Previous position = current + mv (UV units), so history moves 2 output pixels right.
      const float mv[2]={-2.f/OW,0};std::memcpy(data+shift_at+y*motion_pitch+x*8,mv,8);
    }
    put(star_at,color_pitch,10,10,1.f);    // sky star
    put(star_at,color_pitch,32,10,1.f);    // bright point in front of the sky (depth > 0)
    put(star_at,color_pitch,20,6,.015f);   // sub-threshold: 1.5x its ring
    put(empty_at,color_pitch,32,10,1.f);
    for(unsigned y=0;y<OH;++y)for(unsigned x=0;x<OW;++x)put(out_at,out_pitch,x,y,.02f);
    upload->Unmap(0,nullptr);
    D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;HR(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence;HR(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT64 fence_value=0;
    ComPtr<ID3D12CommandAllocator> alloc;HR(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> cmd;HR(d->CreateCommandList(0,qd.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)));
    auto submit=[&] {
      HR(cmd->Close());ID3D12CommandList *lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
      HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);HR(queue->Signal(fence.Get(),++fence_value));
      HR(fence->SetEventOnCompletion(fence_value,event));const auto w=WaitForSingleObject(event,10000);CloseHandle(event);
      Expect(w==WAIT_OBJECT_0,"Software GPU timed out");HR(alloc->Reset());HR(cmd->Reset(alloc.Get(),nullptr));
    };
    auto barrier=[&](ID3D12Resource *r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
      D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};cmd->ResourceBarrier(1,&b);
    };
    // Copies an upload region into a texture in `state` (COPY_DEST when fresh), leaving it in `state`.
    auto copy_in=[&](ID3D12Resource *dst,size_t offset,DXGI_FORMAT format,unsigned w,unsigned h,UINT pitch,
                     D3D12_RESOURCE_STATES state,bool fresh=false) {
      if(!fresh)barrier(dst,state,D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset=offset;src.PlacedFootprint.Footprint={format,w,h,1,pitch};
      D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=dst;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      cmd->CopyTextureRegion(&to,0,0,0,&src,nullptr);barrier(dst,D3D12_RESOURCE_STATE_COPY_DEST,state);
    };
    const auto UAV=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,SRV=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    copy_in(color.Get(),star_at,DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,color_pitch,UAV,true);
    copy_in(depth.Get(),depth_at,DXGI_FORMAT_R32_FLOAT,W,H,depth_pitch,SRV,true);
    copy_in(motion.Get(),still_at,DXGI_FORMAT_R32G32_FLOAT,W,H,motion_pitch,SRV,true);
    copy_in(output.Get(),out_at,DXGI_FORMAT_R32G32B32A32_FLOAT,OW,OH,out_pitch,UAV,true);
    submit();
    auto read_output=[&] {
      barrier(output.Get(),UAV,D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,OW,OH,1,out_pitch};cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,UAV);
      submit();
      std::vector<float> red(OW*OH);float *pixels=nullptr;HR(readback->Map(0,nullptr,reinterpret_cast<void**>(&pixels)));
      for(unsigned y=0;y<OH;++y)for(unsigned x=0;x<OW;++x)red[y*OW+x]=pixels[(y*out_pitch)/4+x*4];
      readback->Unmap(0,nullptr);return red;
    };
    // Exactly the 2x2 block at (x0,y0) must hold `expected`; every other pixel keeps RR's .02.
    auto check=[&](const std::vector<float> &red,unsigned x0,unsigned y0,float expected,float tolerance,const std::string &what) {
      unsigned lit=0,wrong=0;
      for(unsigned y=0;y<OH;++y)for(unsigned x=0;x<OW;++x) {
        const float v=red[y*OW+x];
        if(x>=x0&&x<x0+2&&y>=y0&&y<y0+2)lit+=std::abs(v-expected)<tolerance;else wrong+=std::abs(v-.02f)>1e-6f;
      }
      Expect(lit==4,what+": star block wrong ("+std::to_string(lit)+"/4)");
      Expect(wrong==0,what+": other pixels changed ("+std::to_string(wrong)+")");
    };

    // The "game's" own compute bindings used to re-bind after RR.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=4;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=2;ranges[1].OffsetInDescriptorsFromTableStart=4;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants.Num32BitValues=12;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,error;HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    ComPtr<ID3D12RootSignature> root;HR(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={star_composite_shader,sizeof(star_composite_shader)};
    ComPtr<ID3D12PipelineState> pso;HR(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,6,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ComPtr<ID3D12DescriptorHeap> heap;HR(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    {
      auto cpu=heap->GetCPUDescriptorHandleForHeapStart();const auto step=d->GetDescriptorHandleIncrementSize(hd.Type);
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
      srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
      d->CreateShaderResourceView(color.Get(),&srv,cpu);cpu.ptr+=step;srv.Format=DXGI_FORMAT_R32_FLOAT;
      d->CreateShaderResourceView(depth.Get(),&srv,cpu);cpu.ptr+=step;srv.Format=DXGI_FORMAT_R32G32_FLOAT;
      d->CreateShaderResourceView(motion.Get(),&srv,cpu);cpu.ptr+=step;srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
      d->CreateShaderResourceView(color.Get(),&srv,cpu);cpu.ptr+=step;
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
      d->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);cpu.ptr+=step;
      d->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
    }
    // Zero-group dispatch: a real use of the bindings with no GPU work.
    auto rebind_and_use=[&]{
      ID3D12DescriptorHeap *heaps[]={heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
      cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pso.Get());
      UINT k[12]={};cmd->SetComputeRoot32BitConstants(0,12,k,0);
      cmd->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());cmd->Dispatch(0,0,0);
    };
    sl::Resource color_tag(sl::ResourceType::eTex2d,color.Get(),UAV);
    sl::Resource depth_tag(sl::ResourceType::eTex2d,depth.Get(),SRV);
    sl::Resource motion_tag(sl::ResourceType::eTex2d,motion.Get(),SRV);
    sl::Resource output_tag(sl::ResourceType::eTex2d,output.Get(),UAV);
    sl::Extent in_extent{0,0,W,H},out_extent{0,0,OW,OH};
    auto as_sl=[&]{return reinterpret_cast<sl::CommandBuffer*>(cmd.Get());};
    auto evaluate=[&]{
      const auto life=sl::ResourceLifecycle::eValidUntilPresent;
      sl::ResourceTag tags[]={{&depth_tag,0,life,&in_extent},{&motion_tag,1,life,&in_extent},
        {&color_tag,3,life,&in_extent},{&output_tag,4,life,&out_extent}};
      star_composite::ObserveTags(7,tags,4);
      return star_composite::AfterEvaluate(sl::kFeatureDLSS_RR,as_sl(),0);
    };
    const auto enabled=[]{return star_composite::GetStats().enabled;};
    auto said=[](const char *text){bool found=false;for(const auto &m:star_composite::TakeMessages())found|=m.find(text)!=std::string::npos;return found;};

    // Session 1: automatic activation at launch, but the game fails the check.
    star_composite::Start();
    rebind_and_use();        // bindings from before RR stay valid for the next call
    Expect(!evaluate(),"Composite recorded before the state check");
    Expect(star_composite::GetStats().hooks,"State-restore detours not installed");
    cmd->Dispatch(0,0,0);   // the game uses bindings without re-binding them
    Expect(star_composite::GetStats().unsafe==1,"Use without re-binding not detected");
    for(int i=0;i<3;++i){star_composite::Present();evaluate();rebind_and_use();}
    Expect(!enabled(),"Composite turned on after an unsafe frame");
    star_composite::Present();submit();

    // Session 2: automatic activation at launch; re-binding (twice) and closing (once) verify the game,
    // and the composite turns itself on at the next RR frame.
    star_composite::Start();(void)star_composite::TakeMessages();
    for(int i=0;i<2;++i){evaluate();rebind_and_use();star_composite::Present();}
    Expect(!enabled(),"Composite on after only two verified frames");
    evaluate();submit();star_composite::Present();
    const auto verified=star_composite::GetStats();
    Expect(verified.restored==2 && verified.closed==1 && verified.unsafe==0 && verified.verified,"State-restore counts wrong: "+
      std::to_string(verified.restored)+"/"+std::to_string(verified.closed)+"/"+std::to_string(verified.unsafe));
    // Frame 1, alpha 1 (history reset on enable): the star's excess .99 lands on its 2x2 output block.
    star_composite::SetAlphaForTesting(1.f);
    Expect(evaluate(),"Composite did not turn on automatically after the state check");
    Expect(said("state check passed"),"Automatic turn-on not reported");
    rebind_and_use();star_composite::Present();
    check(read_output(),20,20,.02f+.99f,1e-4f,"Frame 1");
    if(exit_without_stop){std::cout << "Composite active; exiting without Stop().\n";return 0;}
    // Frame 2, alpha .5: the star is gone from the input and the motion says the sky moved, so
    // only the reprojected history shows it, halved, 2 output pixels to the right.
    copy_in(color.Get(),empty_at,DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,color_pitch,UAV);
    copy_in(motion.Get(),shift_at,DXGI_FORMAT_R32G32_FLOAT,W,H,motion_pitch,SRV);
    copy_in(output.Get(),out_at,DXGI_FORMAT_R32G32B32A32_FLOAT,OW,OH,out_pitch,UAV);
    star_composite::SetAlphaForTesting(.5f);
    Expect(evaluate(),"Composite not recorded on frame 2");
    rebind_and_use();star_composite::Present();
    check(read_output(),22,20,.02f+.495f,2e-3f,"Frame 2 history");

    // Frame 3, camera still, F7 "strong" (still weight .015, moving weight still .5): the
    // history barely decays, .495 -> .4876, instead of halving as it would while moving.
    Expect(star_composite::CycleSmoothing().find("strong")!=std::string::npos,"F7 did not select strong");
    copy_in(motion.Get(),still_at,DXGI_FORMAT_R32G32_FLOAT,W,H,motion_pitch,SRV);
    copy_in(output.Get(),out_at,DXGI_FORMAT_R32G32B32A32_FLOAT,OW,OH,out_pitch,UAV);
    Expect(evaluate(),"Composite not recorded on frame 3");
    rebind_and_use();star_composite::Present();
    check(read_output(),22,20,.02f+.495f*.985f,2e-3f,"Frame 3 still-camera smoothing");
    // F7 cycles medium -> strong -> light -> medium.
    Expect(star_composite::CycleSmoothing().find("light")!=std::string::npos,"F7 did not select light");
    Expect(star_composite::CycleSmoothing().find("medium")!=std::string::npos,"F7 did not return to medium");
    // Using bindings without re-binding while enabled switches the composite off.
    evaluate();cmd->Dispatch(0,0,0);
    Expect(!star_composite::GetStats().enabled,"Composite not auto-disabled after an unsafe frame");
    bool reported=false;for(const auto &m:star_composite::TakeMessages())reported|=m.find("auto-disabled")!=std::string::npos;
    Expect(reported,"Auto-disable not reported");
    star_composite::Present();submit();
    star_composite::Stop();
    Expect(MH_Uninitialize()==MH_OK,"MinHook uninit failed");
    const auto errors=api_errors();
    Expect(errors.empty(),"D3D12 debug layer reported errors:\n"+errors);
    std::cout << (validated?"D3D12 debug layer: no errors.\n":"D3D12 debug layer unavailable; API validation not run.\n");
    std::cout << "PASS: state-restore detours installed; composite turns on by itself after 3 verified RR frames, never after an unsafe frame; "
                 "compiled shader adds a sky star (4 output pixels) and leaves non-sky, sub-threshold and background pixels unchanged; "
                 "history accumulates and reprojects with previous = current + mv; auto-disabled on use without re-binding.\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
