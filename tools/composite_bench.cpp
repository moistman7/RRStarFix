// A/B benchmark of the star composite shader on the real GPU with a real RR
// snapshot (depth, motion, input color, output in their game formats).
// Runs the reference v0.7 shader and the current shader for the same frames from
// the same state, times each dispatch with GPU timestamps, and compares the final
// RR output and star history bit for bit.
// usage: RRStarFixCompositeBench <RR snapshot folder> [frames] [--warp]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "star_composite_shader.hpp"
#include "star_composite_v07_shader.hpp"
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
void HR(HRESULT hr,const char *what){if(FAILED(hr))throw std::runtime_error(std::string(what)+" failed: "+std::to_string(hr));}
struct Raw { unsigned w=0,h=0,format=0,pitch=0; std::vector<unsigned char> bytes; };
std::map<unsigned,Raw> LoadSnapshot(const fs::path &dir) {
  std::ifstream manifest(dir/"manifest.csv");std::string line;std::getline(manifest,line);
  std::map<unsigned,Raw> out;
  while(std::getline(manifest,line)) {
    std::stringstream s(line);std::string f[12];for(auto &v:f)std::getline(s,v,',');
    Raw r;r.w=std::stoul(f[2]);r.h=std::stoul(f[3]);r.format=std::stoul(f[4]);r.pitch=std::stoul(f[5]);
    std::ifstream file(dir/(f[0]+"-"+f[1]+".raw"),std::ios::binary);
    r.bytes.assign(std::istreambuf_iterator<char>(file),{});
    if(r.bytes.size()<size_t(r.pitch)*(r.h-1)+r.w)throw std::runtime_error("Truncated "+f[0]);
    out[std::stoul(f[0])]=std::move(r);
  }
  for(unsigned t:{0u,1u,3u,4u})if(!out.contains(t))throw std::runtime_error("Snapshot lacks buffer "+std::to_string(t));
  return out;
}
int main(int argc,char **argv) {
  try {
    if(argc<2)throw std::runtime_error("usage: RRStarFixCompositeBench <RR snapshot folder> [frames] [--warp]");
    const auto snap=LoadSnapshot(argv[1]);
    const unsigned frames=argc>2 && std::string(argv[2]).rfind("--",0)!=0?std::stoul(argv[2]):240;
    bool warp=false,reverse=false;
    for(int i=2;i<argc;++i){warp|=std::string(argv[i])=="--warp";reverse|=std::string(argv[i])=="--reverse";}
    ComPtr<IDXGIFactory6> factory;HR(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    if(warp)HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");
    else HR(factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)),"GPU adapter");
    DXGI_ADAPTER_DESC1 ad{};adapter->GetDesc1(&ad);
    std::wcout << L"Adapter: " << ad.Description << L"\n";
    ComPtr<ID3D12Device> d;HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)),"Device");
    // --setup: time the one-time work the add-on does on the first composited frame.
    for(int i=2;i<argc;++i)if(std::string(argv[i])=="--setup") {
      using clock=std::chrono::steady_clock;auto ms=[](clock::time_point a){return std::chrono::duration<double,std::milli>(clock::now()-a).count();};
      D3D12_DESCRIPTOR_RANGE r[2]{};r[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;r[0].NumDescriptors=4;
      r[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;r[1].NumDescriptors=2;r[1].OffsetInDescriptorsFromTableStart=4;
      D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants.Num32BitValues=12;
      p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[1].DescriptorTable={2,r};
      D3D12_STATIC_SAMPLER_DESC s{};s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;s.MaxLOD=D3D12_FLOAT32_MAX;
      D3D12_ROOT_SIGNATURE_DESC rd{2,p,1,&s,D3D12_ROOT_SIGNATURE_FLAG_NONE};
      auto t=clock::now();ComPtr<ID3DBlob> b,e;HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&b,&e),"Serialize");
      ComPtr<ID3D12RootSignature> root;HR(d->CreateRootSignature(0,b->GetBufferPointer(),b->GetBufferSize(),IID_PPV_ARGS(&root)),"Root");
      std::cout << "root signature: " << ms(t) << " ms\n";
      t=clock::now();D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={star_composite_shader,sizeof(star_composite_shader)};
      ComPtr<ID3D12PipelineState> pso;HR(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)),"PSO");
      std::cout << "pipeline state: " << ms(t) << " ms (driver shader cache may make repeat runs faster)\n";
      t=clock::now();D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,64*6,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
      ComPtr<ID3D12DescriptorHeap> heap;HR(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"Heap");
      std::cout << "descriptor heap: " << ms(t) << " ms\n";
      t=clock::now();D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rdsc{};
      rdsc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rdsc.Width=snap.at(4).w;rdsc.Height=snap.at(4).h;rdsc.DepthOrArraySize=1;rdsc.MipLevels=1;
      rdsc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rdsc.SampleDesc.Count=1;rdsc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      ComPtr<ID3D12Resource> h0,h1;
      HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rdsc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&h0)),"History 0");
      HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rdsc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&h1)),"History 1");
      std::cout << "two history buffers: " << ms(t) << " ms\n";
      return 0;
    }
    const auto &sd=snap.at(0),&sm=snap.at(1),&sc=snap.at(3),&so=snap.at(4);
    if(so.format!=10 || sd.format!=39)throw std::runtime_error("Expected RGBA16F output and R32 depth");
    std::cout << "Input " << sc.w << "x" << sc.h << ", output " << so.w << "x" << so.h << ", frames " << frames << "\n";
    auto typeless=[](unsigned f){return f==39?DXGI_FORMAT_R32_TYPELESS:static_cast<DXGI_FORMAT>(f);};
    auto texture=[&](DXGI_FORMAT f,unsigned w,unsigned h,bool uav,D3D12_RESOURCE_STATES state) {
      D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
      desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=f;desc.SampleDesc.Count=1;
      if(uav)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      ComPtr<ID3D12Resource> r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)),"Texture");return r;
    };
    auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
      D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width=size;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ComPtr<ID3D12Resource> r;HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)),"Buffer");return r;
    };
    const auto SRV=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,UAV=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      DST=D3D12_RESOURCE_STATE_COPY_DEST,SRC=D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto depth=texture(typeless(sd.format),sd.w,sd.h,false,DST),motion=texture(static_cast<DXGI_FORMAT>(sm.format),sm.w,sm.h,false,DST);
    auto color=texture(static_cast<DXGI_FORMAT>(sc.format),sc.w,sc.h,false,DST);
    // One upload buffer holds all four raw snapshot buffers at 512-byte aligned offsets.
    std::vector<const Raw*> raws{&sd,&sm,&sc,&so};std::vector<size_t> at;size_t total=0;
    for(auto *r:raws){at.push_back(total);total=(total+r->bytes.size()+511)&~size_t(511);}
    auto upload=buffer(total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    unsigned char *mapped=nullptr;HR(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"Map upload");
    for(size_t i=0;i<raws.size();++i)std::memcpy(mapped+at[i],raws[i]->bytes.data(),raws[i]->bytes.size());
    upload->Unmap(0,nullptr);
    D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;HR(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"Queue");
    ComPtr<ID3D12CommandAllocator> alloc;HR(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)),"Allocator");
    ComPtr<ID3D12GraphicsCommandList> cmd;HR(d->CreateCommandList(0,qd.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)),"List");
    ComPtr<ID3D12Fence> fence;HR(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");UINT64 fv=0;
    auto submit=[&] {
      HR(cmd->Close(),"Close");ID3D12CommandList *lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
      HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);HR(queue->Signal(fence.Get(),++fv),"Signal");
      HR(fence->SetEventOnCompletion(fv,e),"Event");const auto w=WaitForSingleObject(e,60000);CloseHandle(e);
      if(w!=WAIT_OBJECT_0)throw std::runtime_error("GPU timed out");
      HR(d->GetDeviceRemovedReason(),"Device");HR(alloc->Reset(),"Reset allocator");HR(cmd->Reset(alloc.Get(),nullptr),"Reset list");
    };
    auto barrier=[&](ID3D12Resource *r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
      D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};cmd->ResourceBarrier(1,&x);};
    auto copy_in=[&](ID3D12Resource *dst,size_t i,DXGI_FORMAT f){
      D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset=at[i];src.PlacedFootprint.Footprint={f,raws[i]->w,raws[i]->h,1,raws[i]->pitch};
      D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=dst;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      cmd->CopyTextureRegion(&to,0,0,0,&src,nullptr);};
    const auto depth_view=DXGI_FORMAT_R32_FLOAT;
    copy_in(depth.Get(),0,typeless(sd.format));copy_in(motion.Get(),1,static_cast<DXGI_FORMAT>(sm.format));
    copy_in(color.Get(),2,static_cast<DXGI_FORMAT>(sc.format));
    // RR tags input color as UAV in the game; the shader reads it as an SRV after a transition.
    barrier(depth.Get(),DST,SRV);barrier(motion.Get(),DST,SRV);barrier(color.Get(),DST,SRV);
    submit();
    // Root signature identical to src/star_composite.cpp.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=4;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=2;ranges[1].OffsetInDescriptorsFromTableStart=4;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants.Num32BitValues=12;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,error;HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Serialize");
    ComPtr<ID3D12RootSignature> root;HR(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Root");
    struct Variant{const char *name;const unsigned char *code;size_t size;double median=0,mean=0;std::vector<unsigned char> out,hist;};
    Variant variants[2]={{"v0.7 reference",star_composite_v07_shader,sizeof(star_composite_v07_shader)},
                         {"optimized",star_composite_shader,sizeof(star_composite_shader)}};
    UINT64 tick=0;HR(queue->GetTimestampFrequency(&tick),"Timestamp frequency");
    const DXGI_FORMAT of=static_cast<DXGI_FORMAT>(so.format),hf=DXGI_FORMAT_R16G16B16A16_FLOAT;
    // --reverse runs the optimized shader first, to rule out ordering effects (clocks, caches).
    for(int vi=0;vi<2;++vi) {
      auto &v=variants[reverse?1-vi:vi];
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={v.code,v.size};
      ComPtr<ID3D12PipelineState> pso;HR(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)),"PSO");
      auto output=texture(of,so.w,so.h,true,DST);
      ComPtr<ID3D12Resource> hist[2]={texture(hf,so.w,so.h,true,SRV),texture(hf,so.w,so.h,true,UAV)};
      D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,12,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
      ComPtr<ID3D12DescriptorHeap> heap;HR(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"Heap");
      const UINT step=d->GetDescriptorHandleIncrementSize(hd.Type);
      // Two descriptor sets, one per history ping-pong direction.
      for(int set=0;set<2;++set) {
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(set*6)*step;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format=static_cast<DXGI_FORMAT>(sc.format);d->CreateShaderResourceView(color.Get(),&srv,cpu);cpu.ptr+=step;
        srv.Format=depth_view;d->CreateShaderResourceView(depth.Get(),&srv,cpu);cpu.ptr+=step;
        srv.Format=static_cast<DXGI_FORMAT>(sm.format);d->CreateShaderResourceView(motion.Get(),&srv,cpu);cpu.ptr+=step;
        srv.Format=hf;d->CreateShaderResourceView(hist[set].Get(),&srv,cpu);cpu.ptr+=step;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format=of;d->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);cpu.ptr+=step;
        uav.Format=hf;d->CreateUnorderedAccessView(hist[1-set].Get(),nullptr,&uav,cpu);
      }
      D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2*frames,0};
      ComPtr<ID3D12QueryHeap> queries;HR(d->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)),"Query heap");
      auto times=buffer(8ull*2*frames,D3D12_HEAP_TYPE_READBACK,DST);
      D3D12_RESOURCE_STATES out_state=DST;
      for(unsigned f=0;f<frames;++f) {
        // RR writes a fresh output every frame: restore the snapshot output first (not timed).
        if(out_state!=DST)barrier(output.Get(),out_state,DST);
        copy_in(output.Get(),3,of);barrier(output.Get(),DST,UAV);out_state=UAV;
        const unsigned read=f&1;
        ID3D12DescriptorHeap *heaps[]={heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pso.Get());
        const float fl[6]={float(sc.w)/float(so.w),float(sc.h)/float(so.h),1.f,1.f,.03f,1.f};
        UINT k[12]={sc.w,sc.h,so.w,so.h};std::memcpy(&k[4],fl,sizeof(fl));k[10]=f==0;const float moving=.15f;std::memcpy(&k[11],&moving,4);
        cmd->SetComputeRoot32BitConstants(0,12,k,0);
        auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(read*6)*step;cmd->SetComputeRootDescriptorTable(1,gpu);
        cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2*f);
        cmd->Dispatch((so.w+7)/8,(so.h+7)/8,1);
        cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2*f+1);
        barrier(hist[1-read].Get(),UAV,SRV);barrier(hist[read].Get(),SRV,UAV);
        if(f%16==15)submit();
      }
      cmd->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2*frames,times.Get(),0);
      submit();
      // Read back the final RR output and the history written by the last frame.
      ID3D12Resource *last=hist[frames&1].Get();   // written by frame frames-1, now in SRV state
      auto readback=[&](ID3D12Resource *r,D3D12_RESOURCE_STATES state,DXGI_FORMAT f) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 size=0;const auto desc=r->GetDesc();
        d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&size);
        auto rb=buffer(size,D3D12_HEAP_TYPE_READBACK,DST);barrier(r,state,SRC);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(r,SRC,state);submit();
        std::vector<unsigned char> bytes;unsigned char *p=nullptr;HR(rb->Map(0,nullptr,reinterpret_cast<void**>(&p)),"Map readback");
        const size_t row=size_t(desc.Width)*8;(void)f;
        for(UINT y=0;y<desc.Height;++y)bytes.insert(bytes.end(),p+fp.Offset+size_t(y)*fp.Footprint.RowPitch,p+fp.Offset+size_t(y)*fp.Footprint.RowPitch+row);
        rb->Unmap(0,nullptr);return bytes;
      };
      v.out=readback(output.Get(),UAV,of);v.hist=readback(last,SRV,hf);
      UINT64 *t=nullptr;HR(times->Map(0,nullptr,reinterpret_cast<void**>(&t)),"Map timestamps");
      std::vector<double> ms;for(unsigned f=8;f<frames;++f)ms.push_back(double(t[2*f+1]-t[2*f])*1000.0/double(tick));
      times->Unmap(0,nullptr);
      std::sort(ms.begin(),ms.end());v.median=ms[ms.size()/2];
      double sum=0;for(double x:ms)sum+=x;v.mean=sum/ms.size();
      std::cout << v.name << ": median " << v.median << " ms, mean " << v.mean << " ms per dispatch (frames 8.." << frames-1 << ")\n";
    }
    auto diff=[](const std::vector<unsigned char> &a,const std::vector<unsigned char> &b){
      size_t texels=0;for(size_t i=0;i+8<=a.size()&&i+8<=b.size();i+=8)texels+=std::memcmp(&a[i],&b[i],8)!=0;return texels;};
    const size_t out_diff=diff(variants[0].out,variants[1].out),hist_diff=diff(variants[0].hist,variants[1].hist);
    size_t star_texels=0;for(size_t i=0;i+8<=variants[0].hist.size();i+=8){uint64_t x;std::memcpy(&x,&variants[0].hist[i],8);star_texels+=x!=0;}
    std::cout << "Nonzero star-history texels: " << star_texels << " of " << variants[0].hist.size()/8 << "\n";
    std::cout << "Differing texels: output " << out_diff << ", history " << hist_diff << "\n";
    std::cout << "Speedup: " << variants[0].median/variants[1].median << "x\n";
    if(out_diff||hist_diff){std::cout << "FAIL: optimized shader output differs\n";return 1;}
    std::cout << "PASS: bit-identical output and history\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
