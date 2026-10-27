// Exercises pixel-snapshot safety rules on Windows' software D3D12 adapter:
// per-frame tags, ignored eOnlyValidNow tags, one-time rejection without
// allocation, delayed fence signal, and a single write attempt on failure.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string>
#include "gpu_snapshot.hpp"
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
void HR(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed: " + std::to_string(hr)); }
void Expect(bool condition,const char *what){if(!condition)throw std::runtime_error(what);}
int main(int argc,char **argv) {
  try {
    if(argc!=2 && argc!=3)throw std::runtime_error("Snapshot test output path required");
    // --exit-without-stop: take a snapshot, then leave the process the way the game
    // does when ReShade skips AddonUninit. Must exit cleanly (code 0), not abort.
    const bool exit_without_stop=argc==3 && std::string(argv[2])=="--exit-without-stop";
    if(exit_without_stop){_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);}
    const auto root=fs::path(argv[1])/std::to_string(GetTickCount64());
    ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    // RRSTARFIX_GBV=1 also validates resource states while commands run on the GPU.
    if(char *gbv=nullptr;debug && _dupenv_s(&gbv,nullptr,"RRSTARFIX_GBV")==0 && gbv){const bool on=gbv[0]=='1';free(gbv);
      ComPtr<ID3D12Debug1> d1;if(on && SUCCEEDED(debug.As(&d1))){d1->SetEnableGPUBasedValidation(TRUE);std::cout << "GPU-based validation on.\n";}}
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> d; HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
    // With Windows Graphics Tools installed, any debug-layer error fails the test.
    ComPtr<ID3D12InfoQueue> info;if(debug)d.As(&info);
    auto api_errors=[&]{
      std::string text;if(!info)return text;
      for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);
        std::vector<char> m(n);auto *msg=reinterpret_cast<D3D12_MESSAGE*>(m.data());info->GetMessage(i,msg,&n);
        if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)text+=std::string(msg->pDescription)+"\n";}
      return text;
    };
    auto texture=[&](bool output) {
      D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width=2;desc.Height=2;desc.DepthOrArraySize=1;desc.MipLevels=1;
      desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.SampleDesc.Count=1;
      if(output)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      ComPtr<ID3D12Resource> r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
        output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto input=texture(false),result=texture(true);
    D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;HR(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> idle;HR(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&idle)));
    UINT64 idle_value=0;
    auto wait_idle=[&] {
      HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("CreateEvent failed");
      HR(queue->Signal(idle.Get(),++idle_value));HR(idle->SetEventOnCompletion(idle_value,event));
      const auto wait=WaitForSingleObject(event,10000);CloseHandle(event);
      Expect(wait==WAIT_OBJECT_0,"Software GPU timed out");
    };
    auto list=[&](D3D12_COMMAND_LIST_TYPE type,ComPtr<ID3D12CommandAllocator> &alloc) {
      HR(d->CreateCommandAllocator(type,IID_PPV_ARGS(&alloc)));
      ComPtr<ID3D12GraphicsCommandList> cmd;HR(d->CreateCommandList(0,type,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)));return cmd;
    };
    // Known pixel values, so a saved snapshot can be checked byte for byte.
    const float input_pixels[4][4]={{.25f,.5f,.75f,1},{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    const float output_pixels[4][4]={{2,3,4,5},{6,7,8,9},{10,11,12,13},{14,15,16,17}};
    {
      D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width=1024;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ComPtr<ID3D12Resource> upload;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
      unsigned char *p=nullptr;HR(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)));
      for(int i=0;i<4;++i){std::memcpy(p+(i/2)*256+(i%2)*16,input_pixels[i],16);std::memcpy(p+512+(i/2)*256+(i%2)*16,output_pixels[i],16);}
      upload->Unmap(0,nullptr);
      ComPtr<ID3D12CommandAllocator> alloc;auto cmd=list(D3D12_COMMAND_LIST_TYPE_DIRECT,alloc);
      auto fill=[&](ID3D12Resource *r,UINT64 offset,D3D12_RESOURCE_STATES state){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_DEST};cmd->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset=offset;src.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,2,2,1,256};
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);cmd->ResourceBarrier(1,&b);};
      fill(input.Get(),0,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);fill(result.Get(),512,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      HR(cmd->Close());ID3D12CommandList *lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);wait_idle();
    }
    sl::Resource input_tag(sl::ResourceType::eTex2d,input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource output_tag(sl::ResourceType::eTex2d,result.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource bad_output_tag(sl::ResourceType::eTex2d,result.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    sl::Extent extent{0,0,2,2};
    auto observe=[&](sl::ResourceLifecycle depth_life,sl::Resource *output) {
      const auto life=sl::ResourceLifecycle::eValidUntilPresent;
      sl::ResourceTag tags[]={{&input_tag,0,depth_life,&extent},{&input_tag,1,life,&extent},
        {&input_tag,3,life,&extent},{&input_tag,40,life,&extent},{output,4,life,&extent}};
      gpu_snapshot::ObserveTags(7,tags,5);
    };
    auto as_sl=[](ID3D12GraphicsCommandList *c){return reinterpret_cast<sl::CommandBuffer*>(c);};
    const auto rr=sl::kFeatureDLSS_RR;
    if(exit_without_stop) {
      gpu_snapshot::Start(root/"exit");gpu_snapshot::Arm();
      ComPtr<ID3D12CommandAllocator> alloc;auto cmd=list(D3D12_COMMAND_LIST_TYPE_DIRECT,alloc);
      observe(sl::ResourceLifecycle::eValidUntilPresent,&output_tag);
      auto capture=gpu_snapshot::Begin(rr,as_sl(cmd.Get()));Expect(capture!=nullptr,"Snapshot did not start");
      gpu_snapshot::End(capture,as_sl(cmd.Get()),0,false);capture.reset();
      HR(cmd->Close());ID3D12CommandList *lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);wait_idle();
      gpu_snapshot::Present(queue.Get());gpu_snapshot::Present(queue.Get());wait_idle();
      for(int i=0;i<200 && gpu_snapshot::GetCounters().written==0;++i){gpu_snapshot::Present(queue.Get());Sleep(10);}
      Expect(gpu_snapshot::GetCounters().written==1,"Snapshot not written");
      std::cout << "Snapshot written; exiting without Stop().\n";
      return 0;   // no gpu_snapshot::Stop(): the writer thread was never joined
    }

    // Tags from a previous frame are never used; eOnlyValidNow tags are ignored.
    gpu_snapshot::Start(root/"stale");
    gpu_snapshot::Arm();
    {
      ComPtr<ID3D12CommandAllocator> alloc;auto cmd=list(D3D12_COMMAND_LIST_TYPE_DIRECT,alloc);
      observe(sl::ResourceLifecycle::eValidUntilPresent,&output_tag);
      gpu_snapshot::Present(queue.Get());
      Expect(!gpu_snapshot::Begin(rr,as_sl(cmd.Get())),"Stale tags from a previous frame were used");
      observe(sl::ResourceLifecycle::eOnlyValidNow,&output_tag);
      Expect(!gpu_snapshot::Begin(rr,as_sl(cmd.Get())),"eOnlyValidNow depth tag was used");
      Expect(gpu_snapshot::GetCounters().allocations==0,"Skipped snapshot allocated memory");
      HR(cmd->Close());
    }
    gpu_snapshot::Stop();

    // An unsupported state skips that frame without allocating and does not
    // abandon the mode; a non-direct command list abandons it once.
    gpu_snapshot::Start(root/"reject");
    gpu_snapshot::Arm();
    {
      ComPtr<ID3D12CommandAllocator> alloc;auto cmd=list(D3D12_COMMAND_LIST_TYPE_DIRECT,alloc);
      for(int frame=0;frame<3;++frame) {
        observe(sl::ResourceLifecycle::eValidUntilPresent,&bad_output_tag);
        Expect(!gpu_snapshot::Begin(rr,as_sl(cmd.Get())),"Unsupported output state was accepted");
        gpu_snapshot::Present(queue.Get());
      }
      const auto c=gpu_snapshot::GetCounters();
      Expect(c.allocations==0,"Skipped snapshot allocated readback memory");
      Expect(c.rejected==0,"Unsupported state abandoned the mode");
      ComPtr<ID3D12CommandAllocator> compute_alloc;auto compute=list(D3D12_COMMAND_LIST_TYPE_COMPUTE,compute_alloc);
      for(int frame=0;frame<3;++frame) {
        observe(sl::ResourceLifecycle::eValidUntilPresent,&output_tag);
        Expect(!gpu_snapshot::Begin(sl::kFeatureDLSS,as_sl(compute.Get())),"Compute command list was accepted");
        gpu_snapshot::Present(queue.Get());
      }
      Expect(gpu_snapshot::GetCounters().rejected==1,"Compute rejection not recorded exactly once");
      // RR was never abandoned, so a later valid frame still starts a snapshot.
      observe(sl::ResourceLifecycle::eValidUntilPresent,&output_tag);
      auto later=gpu_snapshot::Begin(rr,as_sl(cmd.Get()));
      Expect(later!=nullptr,"Valid frame after a skipped frame did not start");
      gpu_snapshot::End(later,as_sl(cmd.Get()),0,false);
      HR(cmd->Close());HR(compute->Close());
      ID3D12CommandList *lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
      gpu_snapshot::Present(queue.Get());gpu_snapshot::Present(queue.Get());
      wait_idle();gpu_snapshot::Present(queue.Get());
    }
    gpu_snapshot::Stop();

    // Full snapshot: signalled on the second present, written exactly once.
    auto full=[&](const fs::path &path,bool fail) {
      gpu_snapshot::FailWritesForTesting(fail);
      gpu_snapshot::Start(path);
      gpu_snapshot::Arm();
      ComPtr<ID3D12CommandAllocator> alloc;auto cmd=list(D3D12_COMMAND_LIST_TYPE_DIRECT,alloc);
      observe(sl::ResourceLifecycle::eValidUntilPresent,&output_tag);
      auto capture=gpu_snapshot::Begin(rr,as_sl(cmd.Get()));
      Expect(capture!=nullptr,"Valid snapshot did not start");
      Expect(gpu_snapshot::GetCounters().allocations==5,"Expected five readback buffers");
      // One F9 press captures one evaluation.
      observe(sl::ResourceLifecycle::eValidUntilPresent,&output_tag);
      Expect(!gpu_snapshot::Begin(rr,as_sl(cmd.Get())),"Second evaluation captured without re-arming");
      Expect(gpu_snapshot::Arm().find("armed")!=std::string::npos,"Re-arming failed");
      Expect(gpu_snapshot::Arm().find("already")!=std::string::npos,"Double arming not reported");
      gpu_snapshot::End(capture,as_sl(cmd.Get()),0,false);
      HR(cmd->Close());ID3D12CommandList *lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
      wait_idle();
      gpu_snapshot::Present(queue.Get());
      Expect(gpu_snapshot::GetCounters().signaled==0,"Fence signalled on the first present");
      gpu_snapshot::Present(queue.Get());
      Expect(gpu_snapshot::GetCounters().signaled==1,"Fence not signalled on the second present");
      wait_idle();
      for(int frame=0;frame<5;++frame)gpu_snapshot::Present(queue.Get());
      gpu_snapshot::Stop();
      gpu_snapshot::FailWritesForTesting(false);
      return gpu_snapshot::GetCounters();
    };
    auto count_raw=[](const fs::path &path) {
      unsigned raw=0;
      for(const auto &entry:fs::recursive_directory_iterator(path))raw+=entry.path().extension()==".raw";
      return raw;
    };
    const auto ok=full(root/"write",false);
    Expect(ok.write_attempts==1 && ok.written==1 && ok.pending==0,"Snapshot was not written exactly once");
    Expect(count_raw(root/"write")==5,"Expected four input images and one output image");
    // Saved pixels match what the GPU held: input color before, output after (row pitch 256).
    auto pixels_match=[](const fs::path &file,const float (&expected)[4][4]) {
      std::ifstream in(file,std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});
      if(bytes.size()<256+32)return false;
      for(int i=0;i<4;++i)if(std::memcmp(bytes.data()+(i/2)*256+(i%2)*16,expected[i],16))return false;
      return true;};
    unsigned verified=0;
    for(const auto &entry:fs::recursive_directory_iterator(root/"write")) {
      if(entry.path().filename()=="3-before.raw")verified+=pixels_match(entry.path(),input_pixels);
      if(entry.path().filename()=="4-after.raw")verified+=pixels_match(entry.path(),output_pixels);
    }
    Expect(verified==2,"Saved snapshot pixels differ from the GPU contents");
    const auto bad=full(root/"fail",true);
    Expect(bad.write_attempts==1 && bad.written==0 && bad.pending==0,"Failed write was retried or kept pending");
    Expect(count_raw(root/"fail")==0,"Failed write left pixel files");
    const auto errors=api_errors();
    if(!errors.empty())throw std::runtime_error("D3D12 debug layer reported errors:\n"+errors);
    std::cout << (info?"D3D12 debug layer: no errors.\n":"D3D12 debug layer unavailable; API validation not run.\n");
    std::cout << "PASS: per-frame tags, eOnlyValidNow ignored, rejection without allocation or retry, "
                 "direct-list only, fence on second present, saved pixels byte-exact, single write attempt on success and failure.\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
