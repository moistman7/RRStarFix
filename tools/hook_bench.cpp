// CPU cost of the star composite's state-check detours per D3D12 command-list
// call, single-threaded and with several threads recording at once (as a game
// does). Measures SetPipelineState calls before the detours are installed and
// after, on Windows' software adapter (the detours sit in the D3D12 runtime's
// command-list code, which is the same for every adapter).
// usage: RRStarFixHookBench [threads] [calls per thread]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "star_composite.hpp"
#include "star_composite_shader.hpp"
using Microsoft::WRL::ComPtr;
void HR(HRESULT hr,const char *what){if(FAILED(hr))throw std::runtime_error(std::string(what)+" failed");}
int main(int argc,char **argv) {
  try {
    const unsigned threads=argc>1?std::stoul(argv[1]):8,calls=argc>2?std::stoul(argv[2]):2000000;
    ComPtr<IDXGIFactory4> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");
    ComPtr<IDXGIAdapter> adapter;HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");
    ComPtr<ID3D12Device> d;HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)),"Device");
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=4;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=2;ranges[1].OffsetInDescriptorsFromTableStart=4;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants.Num32BitValues=12;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,error;HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Serialize");
    ComPtr<ID3D12RootSignature> root;HR(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={star_composite_shader,sizeof(star_composite_shader)};
    ComPtr<ID3D12PipelineState> pso;HR(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)),"PSO");
    // Each thread records SetPipelineState into its own list, resetting every 50k calls.
    auto run=[&](unsigned n) {
      std::vector<std::thread> workers;std::vector<double> ns(n);
      for(unsigned t=0;t<n;++t)workers.emplace_back([&,t]{
        ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> l;
        HR(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Allocator");
        HR(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)),"List");
        const auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<calls;++i) {
          l->SetPipelineState(pso.Get());
          if(i%50000==49999){l->Close();a->Reset();l->Reset(a.Get(),nullptr);}
        }
        ns[t]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/calls;
        l->Close();
      });
      for(auto &w:workers)w.join();
      return *std::max_element(ns.begin(),ns.end());
    };
    auto best=[&](unsigned n){double b=1e30;for(int i=0;i<3;++i)b=std::min(b,run(n));return b;};
    const double before1=best(1),beforeN=best(threads);
    // Install the detours the same way the add-on does: one RR evaluation with all tags.
    auto texture=[&](DXGI_FORMAT f,bool uav){
      D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
      desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=16;desc.Height=16;desc.DepthOrArraySize=1;desc.MipLevels=1;
      desc.Format=f;desc.SampleDesc.Count=1;if(uav)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      ComPtr<ID3D12Resource> r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"Texture");return r;};
    auto depth=texture(DXGI_FORMAT_R32_TYPELESS,false),motion=texture(DXGI_FORMAT_R16G16_FLOAT,false),color=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    sl::Resource dt(sl::ResourceType::eTex2d,depth.Get(),0),mt(sl::ResourceType::eTex2d,motion.Get(),0),ct(sl::ResourceType::eTex2d,color.Get(),0);
    sl::Extent e{0,0,16,16};const auto life=sl::ResourceLifecycle::eValidUntilPresent;
    sl::ResourceTag tags[]={{&dt,0,life,&e},{&mt,1,life,&e},{&ct,3,life,&e},{&ct,4,life,&e}};
    if(MH_Initialize()!=MH_OK)throw std::runtime_error("MinHook");
    star_composite::Start();star_composite::ObserveTags(1,tags,4);
    {
      ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> l;
      HR(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Allocator");
      HR(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)),"List");
      star_composite::AfterEvaluate(sl::kFeatureDLSS_RR,reinterpret_cast<sl::CommandBuffer*>(l.Get()),0);l->Close();
    }
    if(!star_composite::GetStats().hooks)throw std::runtime_error("Detours not installed");
    const double after1=best(1),afterN=best(threads);
    star_composite::Present();star_composite::Stop();MH_Uninitialize();
    std::cout << "SetPipelineState ns/call (best of 3, slowest thread):\n"
              << "  1 thread : " << before1 << " without detours, " << after1 << " with  (+" << after1-before1 << ")\n"
              << "  " << threads << " threads: " << beforeN << " without detours, " << afterN << " with  (+" << afterN-beforeN << ")\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
