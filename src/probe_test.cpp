// Exercise actual MinHook trampolines in an offline process, with fake Streamline exports.
#include "sl_probe.cpp"
#include <iostream>
#include <stdexcept>
struct Token : sl::FrameToken { operator uint32_t() const override { return 17; } };
std::atomic<unsigned> tag_calls{0}, constants_calls{0}, evaluate_calls{0};
const sl::ResourceTag *expected_tags=nullptr;
const sl::Constants *expected_constants=nullptr;
const sl::FrameToken *expected_token=nullptr;
std::atomic<bool> arguments_ok{true};
extern "C" __declspec(dllexport) __declspec(noinline)
sl::Result ProbeFakeTag(const sl::ViewportHandle &v,const sl::ResourceTag *t,uint32_t n,sl::CommandBuffer *c) {
  ++tag_calls;if(static_cast<unsigned>(v)!=7 || t!=expected_tags || n!=2 || c!=nullptr)arguments_ok.store(false);
  return sl::Result::eErrorIO;
}
extern "C" __declspec(dllexport) __declspec(noinline)
sl::Result ProbeFakeConstants(const sl::Constants &c,const sl::FrameToken &f,const sl::ViewportHandle &v) {
  ++constants_calls;if(&c!=expected_constants || &f!=expected_token || static_cast<unsigned>(v)!=7)arguments_ok.store(false);
  return sl::Result::eErrorDriverOutOfDate;
}
extern "C" __declspec(dllexport) __declspec(noinline)
sl::Result ProbeFakeEvaluate(sl::Feature feature,const sl::FrameToken &f,const sl::BaseStructure **inputs,uint32_t n,sl::CommandBuffer *c) {
  ++evaluate_calls;if(feature!=sl::kFeatureDLSS_RR || &f!=expected_token || inputs!=nullptr || n!=0 || c!=nullptr)arguments_ok.store(false);
  return sl::Result::eErrorIO;
}
void Check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
int main(int argc,char **argv) {
  try {
    Check(argc==2,"Output directory required");
    sl_probe::Start(std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId()),true);
    sl::ViewportHandle viewport(7);Token token;expected_token=&token;
    sl::Resource depth(sl::ResourceType::eTex2d,nullptr);
    depth.width=2;depth.height=2;
    sl::ResourceTag tags[]={
      {&depth,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent},
      {nullptr,sl::kBufferTypeColorBeforeTransparency,sl::ResourceLifecycle::eValidUntilPresent}};
    expected_tags=tags;
    sl::Constants constants{};constants.depthInverted=sl::Boolean::eTrue;
    constants.cameraMotionIncluded=sl::Boolean::eTrue;constants.mvecScale={1,1};expected_constants=&constants;
    std::vector<unsigned char> original(sizeof(constants));std::memcpy(original.data(),&constants,sizeof(constants));
    auto status=MH_Initialize();Check(status==MH_OK,"MinHook initialization failed");sl_probe::minhook_ready=true;
    const auto module=GetModuleHandleW(nullptr);
    sl_probe::Hook(module,"ProbeFakeTag",reinterpret_cast<void*>(sl_probe::HookTag),&sl_probe::real_tag);
    sl_probe::Hook(module,"ProbeFakeConstants",reinterpret_cast<void*>(sl_probe::HookConstants),&sl_probe::real_constants);
    sl_probe::Hook(module,"ProbeFakeEvaluate",reinterpret_cast<void*>(sl_probe::HookEvaluate),&sl_probe::real_evaluate);
    Check(sl_probe::hooks.size()==3,"Expected 3 hooks");
    auto tag=reinterpret_cast<PFun_slSetTag*>(GetProcAddress(module,"ProbeFakeTag"));
    auto set_constants=reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(module,"ProbeFakeConstants"));
    auto evaluate=reinterpret_cast<PFun_slEvaluateFeature*>(GetProcAddress(module,"ProbeFakeEvaluate"));
    Check(tag(viewport,tags,2,nullptr)==sl::Result::eErrorIO,"Tag result altered");
    Check(set_constants(constants,token,viewport)==sl::Result::eErrorDriverOutOfDate,"Constants result altered");
    Check(evaluate(sl::kFeatureDLSS_RR,token,nullptr,0,nullptr)==sl::Result::eErrorIO,"Evaluation result altered");
    Check(arguments_ok.load() && tag_calls==1 && constants_calls==1 && evaluate_calls==1,"Arguments/call counts changed");
    Check(std::memcmp(original.data(),&constants,sizeof(constants))==0,"Input constants modified");
    const auto description=sl_probe::Describe(7,tags,2,false);
    Check(description.find("type=40")!=std::string::npos,"Transparency tag missing");
    Check(sl_probe::Describe(7,nullptr,1,false).find("unavailable")!=std::string::npos,"Null tags mishandled");
    // Formatting stops only once the 256-line log is full; calls are still forwarded.
    Check(!sl_probe::log_full.load(),"Log reported full too early");
    for(int i=0;i<300;++i)sl_probe::Write("filler "+std::to_string(i));
    Check(sl_probe::seen.size()==256 && sl_probe::log_full.load(),"Full log not detected at 256 lines");
    Check(tag(viewport,tags,2,nullptr)==sl::Result::eErrorIO && tag_calls==2,"Call not forwarded with a full log");
    sl_probe::Stop();
    Check(tag(viewport,tags,2,nullptr)==sl::Result::eErrorIO && tag_calls==3,"Original export not restored");
    Check(sl_probe::active.load()==0,"Active hook count leaked");
    Check(sl_probe::TakeStatus().empty(),"Hook status reported without attaching to Streamline");
    // Diagnostics off (the release default): no folder, no log, no formatting.
    const auto quiet=std::filesystem::path(argv[1])/("quiet-"+std::to_string(GetCurrentProcessId()))/"InputProbes";
    sl_probe::Start(quiet,false);
    Check(!std::filesystem::exists(quiet) && !std::filesystem::exists(quiet.parent_path()/"PixelSnapshots") &&
          sl_probe::log_full.load() && !sl_probe::file.is_open(),"Diagnostics-off start created files");
    sl_probe::Stop();
    std::cout << "PASS: live offline trampolines forward arguments/results unchanged; constants immutable, null tags handled, hooks restored; full-log cutoff; diagnostics off writes no files.\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
