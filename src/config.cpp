// RRStarFix.ini: settings, hotkeys and the diagnostics switch.
#include "config.hpp"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>
namespace config {
namespace {
constexpr const wchar_t *section=L"RRStarFix";
constexpr const char *smoothing_names[3]={"light","medium","strong"};
constexpr const char *default_file=
  "; RRStarFix settings. Changes take effect the next time the game starts.\n"
  "; Star fix always enabled after its safety check. F7 smoothing is saved automatically.\n"
  "[RRStarFix]\n"
  "; still-camera smoothing: light, medium or strong (more smoothing = less twinkle, slower fade-in)\n"
  "Smoothing=medium\n"
  "; hotkeys: F1-F24, A-Z, 0-9, PageUp, PageDown, Home, End, Insert, Delete, Pause, ScrollLock,\n"
  "; NumPad0-NumPad9, a hex virtual-key code such as 0x77, or None\n"
  "SmoothingKey=F7\n"
  "; 1 = developer diagnostics: Streamline metadata log, SnapshotKey pixel snapshots (up to 8 per\n"
  "; session, ~80 MB each) and TraceKey draw traces, written to %LOCALAPPDATA%\\RRStarFix\n"
  "Diagnostics=0\n"
  "SnapshotKey=F9\n"
  "TraceKey=F10\n";
std::string Lower(std::string s){for(auto &c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;}
std::string Trim(const std::string &s) {
  const auto a=s.find_first_not_of(" \t\r\n"),b=s.find_last_not_of(" \t\r\n");
  return a==std::string::npos?std::string():s.substr(a,b-a+1);
}
std::string Read(const std::filesystem::path &ini,const wchar_t *key) {
  wchar_t value[256]{};
  GetPrivateProfileStringW(section,key,L"\x01",value,256,ini.c_str());
  if(value[0]==L'\x01')return "\x01";   // key absent
  std::string out;for(const wchar_t *p=value;*p;++p)out+=*p<128?char(*p):'?';
  return Trim(out);
}
struct Named{const char *name,*display;int key;};
constexpr Named named[]={{"pageup","PageUp",VK_PRIOR},{"pagedown","PageDown",VK_NEXT},{"home","Home",VK_HOME},{"end","End",VK_END},
  {"insert","Insert",VK_INSERT},{"delete","Delete",VK_DELETE},{"pause","Pause",VK_PAUSE},{"scrolllock","ScrollLock",VK_SCROLL}};
}
int ParseKey(const std::string &raw) {
  const auto name=Lower(Trim(raw));
  if(name.empty() || name=="none")return 0;
  if(name.size()>2 && name.rfind("0x",0)==0) {
    if(name.size()>4 || !std::all_of(name.begin()+2,name.end(),[](char c){return std::isxdigit(static_cast<unsigned char>(c));}))return -1;
    const int key=std::stoi(name.substr(2),nullptr,16);
    return key>0 && key<255?key:-1;
  }
  if(name.size()==1 && std::isalnum(static_cast<unsigned char>(name[0])))return std::toupper(static_cast<unsigned char>(name[0]));
  if(name[0]=='f' && name.size()<=3 && std::all_of(name.begin()+1,name.end(),[](char c){return std::isdigit(static_cast<unsigned char>(c));})) {
    const int n=std::stoi(name.substr(1));
    return n>=1 && n<=24?VK_F1+n-1:-1;
  }
  if(name.rfind("numpad",0)==0 && name.size()==7 && std::isdigit(static_cast<unsigned char>(name[6])))return VK_NUMPAD0+(name[6]-'0');
  for(const auto &n:named)if(name==n.name)return n.key;
  return -1;
}
std::string KeyName(int key) {
  if(key<=0)return "None";
  if(key>=VK_F1 && key<=VK_F24)return "F"+std::to_string(key-VK_F1+1);
  if((key>='A' && key<='Z') || (key>='0' && key<='9'))return std::string(1,char(key));
  if(key>=VK_NUMPAD0 && key<=VK_NUMPAD9)return "NumPad"+std::to_string(key-VK_NUMPAD0);
  for(const auto &n:named)if(key==n.key)return n.display;
  char hex[8];snprintf(hex,sizeof(hex),"0x%02X",key);return hex;
}
const char *SmoothingName(unsigned level){return smoothing_names[level<3?level:1];}
Settings Load(const std::filesystem::path &ini,std::vector<std::string> &warnings) {
  Settings s;
  std::error_code ec;
  if(!std::filesystem::exists(ini,ec)) {
    std::ofstream file(ini);file << default_file;
    if(!file)warnings.push_back("Could not create "+ini.filename().string()+"; using defaults.");
  }
  auto flag=[&](const wchar_t *key,const char *label,bool &out) {
    const auto v=Read(ini,key);if(v=="\x01")return;
    if(v=="1")out=true;else if(v=="0")out=false;else warnings.push_back(std::string(label)+"="+v+" is not 0 or 1; keeping "+(out?"1":"0")+".");
  };
  auto key=[&](const wchar_t *name,const char *label,int &out) {
    const auto v=Read(ini,name);if(v=="\x01")return;
    const int k=ParseKey(v);
    if(k<0)warnings.push_back(std::string(label)+"="+v+" is not a key name; keeping "+KeyName(out)+".");else out=k;
  };
  flag(L"Diagnostics","Diagnostics",s.diagnostics);
  const auto smoothing=Lower(Read(ini,L"Smoothing"));
  if(smoothing!="\x01") {
    bool found=false;for(unsigned i=0;i<3;++i)if(smoothing==smoothing_names[i]){s.smoothing=i;found=true;}
    if(!found)warnings.push_back("Smoothing="+smoothing+" is not light, medium or strong; keeping medium.");
  }
  key(L"SmoothingKey","SmoothingKey",s.smoothing_key);
  key(L"SnapshotKey","SnapshotKey",s.snapshot_key);key(L"TraceKey","TraceKey",s.trace_key);
  return s;
}
bool SaveState(const std::filesystem::path &ini,unsigned smoothing) {
  std::wstring level;for(const char *p=SmoothingName(smoothing);*p;++p)level+=wchar_t(*p);
  return WritePrivateProfileStringW(section,L"Smoothing",level.c_str(),ini.c_str());
}
}
