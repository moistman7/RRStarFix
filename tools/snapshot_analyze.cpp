// Offline analysis of one v0.4 pixel snapshot (RR-* or SR-* folder).
// Reports sky depth/motion statistics, star-like peaks in the input color,
// color-before-transparency and output, and how many input peaks survive the
// feature evaluation in the same frame. Writes tone-mapped PNG previews.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
float Half(uint16_t h) {
  const uint32_t sign=(h&0x8000u)<<16,exp=(h>>10)&0x1f,man=h&0x3ff;uint32_t bits;
  if(exp==0) {
    if(!man)bits=sign;
    else {int e=-1;uint32_t m=man;do{++e;m<<=1;}while(!(m&0x400));bits=sign|((112-e)<<23)|((m&0x3ff)<<13);}
  } else if(exp==31)bits=sign|0x7f800000u|(man<<13);
  else bits=sign|((exp+112)<<23)|(man<<13);
  float f;std::memcpy(&f,&bits,4);return f;
}
struct Image { unsigned w=0,h=0,channels=0; std::vector<float> data; float at(unsigned x,unsigned y,unsigned c=0)const{return data[(size_t(y)*w+x)*channels+c];} };
Image Load(const fs::path &dir,const std::string &name,unsigned w,unsigned h,unsigned format,unsigned pitch) {
  std::ifstream file(dir/name,std::ios::binary);if(!file)throw std::runtime_error("Missing "+name);
  std::vector<unsigned char> raw((std::istreambuf_iterator<char>(file)),{});
  Image img;img.w=w;img.h=h;
  img.channels=format==10?4:format==34?2:1;
  if(format!=10 && format!=34 && format!=39 && format!=40 && format!=41)throw std::runtime_error("Unsupported format "+std::to_string(format));
  img.data.resize(size_t(w)*h*img.channels);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<img.channels;++c) {
    float v;
    if(format==39||format==40||format==41)std::memcpy(&v,&raw[size_t(y)*pitch+x*4],4);
    else {uint16_t hv;std::memcpy(&hv,&raw[size_t(y)*pitch+(x*img.channels+c)*2],2);v=Half(hv);}
    img.data[(size_t(y)*w+x)*img.channels+c]=v;
  }
  return img;
}
float Luma(const Image &c,unsigned x,unsigned y){return .2126f*c.at(x,y,0)+.7152f*c.at(x,y,1)+.0722f*c.at(x,y,2);}
// Minimal PNG writer using stored (uncompressed) deflate blocks.
uint32_t Crc(const unsigned char *p,size_t n,uint32_t c=0xffffffffu){
  static std::array<uint32_t,256> t=[]{std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;++i){uint32_t c=i;for(int k=0;k<8;++k)c=c&1?0xedb88320u^(c>>1):c>>1;t[i]=c;}return t;}();
  for(size_t i=0;i<n;++i)c=t[(c^p[i])&0xff]^(c>>8);return c;
}
void Png(const fs::path &path,unsigned w,unsigned h,const std::vector<unsigned char> &rgb) {
  std::vector<unsigned char> raw;raw.reserve(size_t(h)*(w*3+1));
  for(unsigned y=0;y<h;++y){raw.push_back(0);raw.insert(raw.end(),rgb.begin()+size_t(y)*w*3,rgb.begin()+size_t(y+1)*w*3);}
  std::vector<unsigned char> z{0x78,0x01};uint32_t a=1,b=0;
  for(unsigned char v:raw){a=(a+v)%65521;b=(b+a)%65521;}
  for(size_t i=0;i<raw.size();i+=65535){
    const size_t n=std::min<size_t>(65535,raw.size()-i);z.push_back(i+n==raw.size());
    z.push_back(n&0xff);z.push_back(n>>8);z.push_back(~n&0xff);z.push_back((~n>>8)&0xff);
    z.insert(z.end(),raw.begin()+i,raw.begin()+i+n);
  }
  const uint32_t adler=(b<<16)|a;for(int s=24;s>=0;s-=8)z.push_back((adler>>s)&0xff);
  std::ofstream f(path,std::ios::binary);
  auto be=[&](uint32_t v){using u8=unsigned char;u8 d[4]={u8(v>>24),u8(v>>16),u8(v>>8),u8(v)};f.write((char*)d,4);};
  auto chunk=[&](const char *type,const std::vector<unsigned char> &d){
    be(uint32_t(d.size()));std::vector<unsigned char> td(type,type+4);td.insert(td.end(),d.begin(),d.end());
    f.write((char*)td.data(),td.size());be(Crc(td.data(),td.size())^0xffffffffu);};
  const unsigned char sig[8]={137,80,78,71,13,10,26,10};f.write((char*)sig,8);
  std::vector<unsigned char> ihdr(13);for(int i=0;i<4;++i){ihdr[i]=(w>>(24-8*i))&0xff;ihdr[4+i]=(h>>(24-8*i))&0xff;}
  ihdr[8]=8;ihdr[9]=2;chunk("IHDR",ihdr);chunk("IDAT",z);chunk("IEND",{});
}
unsigned char Srgb(float v){v=std::clamp(v,0.f,1.f);return (unsigned char)std::lround(255*(v<=.0031308f?12.92f*v:1.055f*std::pow(v,1/2.4f)-.055f));}
float Percentile(std::vector<float> v,double p){if(v.empty())return 0;const size_t k=size_t(p*(v.size()-1));std::nth_element(v.begin(),v.begin()+k,v.end());return v[k];}
// Color preview of a region, scaled so that `white` maps to 1, optionally downsampled.
void ColorPng(const fs::path &path,const Image &c,unsigned x0,unsigned y0,unsigned w,unsigned h,float white,unsigned step,unsigned zoom=1) {
  const unsigned ow=w/step*zoom,oh=h/step*zoom;std::vector<unsigned char> rgb(size_t(ow)*oh*3);
  for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x)for(unsigned k=0;k<3;++k){
    const unsigned sx=x0+(x/zoom)*step,sy=y0+(y/zoom)*step;
    rgb[(size_t(y)*ow+x)*3+k]=Srgb(c.at(sx,sy,k)/white);}
  Png(path,ow,oh,rgb);
}
struct Peak{unsigned x,y;float value,ring;};
// Local maxima at least `ratio` times brighter than the mean of the ring at distance r..2r.
std::vector<Peak> Peaks(const Image &c,const std::function<bool(unsigned,unsigned)> &sky,unsigned r,float ratio,float floor_value) {
  std::vector<Peak> out;const int R=int(2*r);
  for(unsigned y=R;y+R<c.h;++y)for(unsigned x=R;x+R<c.w;++x) {
    if(!sky(x,y))continue;const float v=Luma(c,x,y);if(!(v>floor_value))continue;bool max=true;
    for(int dy=-1;dy<=1&&max;++dy)for(int dx=-1;dx<=1;++dx)if((dx||dy)&&Luma(c,x+dx,y+dy)>v){max=false;break;}
    if(!max)continue;double sum=0;unsigned n=0;
    for(int dy=-R;dy<=R;++dy)for(int dx=-R;dx<=R;++dx){const int d=std::max(std::abs(dx),std::abs(dy));if(d<int(r))continue;sum+=Luma(c,x+dx,y+dy);++n;}
    const float ring=float(sum/n);
    if(std::isfinite(v)&&v>=ratio*std::max(ring,1e-6f))out.push_back({x,y,v,ring});
  }
  return out;
}
std::map<unsigned,Image> LoadSnapshot(const fs::path &dir) {
  std::ifstream manifest(dir/"manifest.csv");std::string line;std::getline(manifest,line);
  std::map<unsigned,Image> img;
  while(std::getline(manifest,line)) {
    std::stringstream s(line);std::string f[12];for(auto &v:f)std::getline(s,v,',');
    const unsigned type=std::stoul(f[0]);
    img[type]=Load(dir,f[0]+"-"+f[1]+".raw",std::stoul(f[2]),std::stoul(f[3]),std::stoul(f[4]),std::stoul(f[5]));
  }
  return img;
}
// Star measurements in one frame: peak, excess energy over the 3x3 and its centroid.
struct Star { float peak=0,energy=0,cx=0,cy=0,ring=0; };
Star Measure(const Image &c,unsigned x,unsigned y) {
  Star s;double sum=0;unsigned n=0;
  for(int dy=-4;dy<=4;++dy)for(int dx=-4;dx<=4;++dx)if(std::max(std::abs(dx),std::abs(dy))>=2){sum+=Luma(c,x+dx,y+dy);++n;}
  s.ring=float(sum/n);s.peak=Luma(c,x,y);double wx=0,wy=0;
  for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const float e=std::max(0.f,Luma(c,x+dx,y+dy)-s.ring);s.energy+=e;wx+=e*(x+dx);wy+=e*(y+dy);}
  if(s.energy>0){s.cx=float(wx/s.energy);s.cy=float(wy/s.energy);}
  return s;
}
// Frame-to-frame stability of stars found in A, matched in B by nearest local maximum.
int Compare(const fs::path &a_dir,const fs::path &b_dir) {
  auto A=LoadSnapshot(a_dir),B=LoadSnapshot(b_dir);
  const auto &ca=A.at(3),&cb=B.at(3),&da=A.at(0),&db=B.at(0);const unsigned W=ca.w,H=ca.h;
  size_t moved=0,solid=0;for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)if(da.at(x,y)>0){++solid;moved+=std::abs(da.at(x,y)-db.at(x,y))>1e-5f*std::max(1.f,da.at(x,y)*1e4f);}
  size_t nonzero=0;for(size_t i=0;i<B.at(1).data.size();++i)nonzero+=B.at(1).data[i]!=0;
  std::cout << "non-sky depth changed A->B: " << (solid?100.0*moved/solid:0) << "%  motion nonzero in B: " << nonzero << " values\n";
  std::vector<float> sky;for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)if(da.at(x,y)<1e-6f)sky.push_back(Luma(ca,x,y));
  auto is_sky=[&](unsigned x,unsigned y){return da.at(x,y)<1e-6f;};
  const auto peaks=Peaks(ca,is_sky,2,2.f,std::max(1e-5f,1.5f*Percentile(sky,.5)));
  std::vector<float> peak_ratio,energy_ratio,dx,dy;size_t matched=0,detected_b=0;
  for(const auto &p:peaks) {
    if(p.x<8||p.y<8||p.x+8>=W||p.y+8>=H)continue;
    unsigned bx=p.x,by=p.y;float best=-1;
    for(int oy=-2;oy<=2;++oy)for(int ox=-2;ox<=2;++ox){const float v=Luma(cb,p.x+ox,p.y+oy);if(v>best){best=v;bx=p.x+ox;by=p.y+oy;}}
    const auto sa=Measure(ca,p.x,p.y),sb=Measure(cb,bx,by);
    if(sb.peak<1.5f*std::max(sb.ring,1e-6f))continue;
    ++matched;detected_b+=sb.peak>=2.f*std::max(sb.ring,1e-6f);
    peak_ratio.push_back((sb.peak-sb.ring)/std::max(1e-9f,sa.peak-sa.ring));
    energy_ratio.push_back(sb.energy/std::max(1e-9f,sa.energy));
    dx.push_back(sb.cx-sa.cx);dy.push_back(sb.cy-sa.cy);
  }
  auto spread=[](const std::vector<float> &v,const char *name){
    std::cout << name << " p10 " << Percentile(v,.1) << " p50 " << Percentile(v,.5) << " p90 " << Percentile(v,.9);
    size_t off=0;for(float r:v)off+=r<0.7f||r>1.f/0.7f;std::cout << "  outside +/-30%: " << (v.empty()?0:100.0*off/v.size()) << "%\n";};
  std::cout << "stars in A: " << peaks.size() << "  matched in B: " << matched << "  still >=2x in B: " << detected_b << "\n";
  spread(peak_ratio,"peak excess B/A:");spread(energy_ratio,"3x3 energy B/A: ");
  double mx=0,my=0;for(size_t i=0;i<dx.size();++i){mx+=dx[i];my+=dy[i];}
  if(!dx.empty()){mx/=dx.size();my/=dy.size();}
  std::vector<float> resid;for(size_t i=0;i<dx.size();++i)resid.push_back(float(std::hypot(dx[i]-mx,dy[i]-my)));
  std::cout << "centroid shift B-A mean (" << mx << ", " << my << ") px; per-star deviation from mean p50 " << Percentile(resid,.5) << " p90 " << Percentile(resid,.9) << "\n";
  // Output stability: excess of each A star's output neighbourhood over the output sky median, A vs B.
  if(A.contains(4) && B.contains(4)) {
    const auto &oa=A.at(4),&ob=B.at(4);const unsigned OW=oa.w,OH=oa.h;
    auto median=[&](const Image &o,const Image &d){std::vector<float> v;for(unsigned y=0;y<OH;y+=3)for(unsigned x=0;x<OW;x+=3)
      if(d.at(std::min(W-1,unsigned((x+.5)*W/OW)),std::min(H-1,unsigned((y+.5)*H/OH)))<1e-6f)v.push_back(Luma(o,x,y));return Percentile(v,.5);};
    const float ma=median(oa,da),mb=median(ob,db);std::vector<float> ratio;
    for(const auto &p:peaks) {
      const int ox=int((p.x+.5)*OW/W),oy=int((p.y+.5)*OH/H);if(ox<4||oy<4||ox+4>=int(OW)||oy+4>=int(OH))continue;
      double ea=0,eb=0;for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){ea+=std::max(0.f,Luma(oa,ox+dx,oy+dy)-ma);eb+=std::max(0.f,Luma(ob,ox+dx,oy+dy)-mb);}
      if(ea>0)ratio.push_back(float(eb/ea));
    }
    spread(ratio,"output 5x5 excess B/A:");
  }
  // Sky motion vectors (one frame's motion, in render pixels) in each snapshot.
  for(int k=0;k<2;++k) {
    const auto &d=k?db:da;const auto &m=(k?B:A).at(1);double sx=0,sy=0;size_t n=0,nz=0;
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)if(d.at(x,y)<1e-6f){const float vx=m.at(x,y,0),vy=m.at(x,y,1);
      if(!std::isfinite(vx)||!std::isfinite(vy))continue;++n;nz+=vx!=0||vy!=0;sx+=vx;sy+=vy;}
    std::cout << (k?"B":"A") << " sky motion mean (" << (n?sx/n*W:0) << ", " << (n?sy/n*H:0) << ") px, nonzero "
      << (n?100.0*nz/n:0) << "%\n";
  }
  return 0;
}
// Alignment of the composited star layer with this frame's input stars, along the
// sky's motion direction. A correctly reprojected history peaks at offset 0; a
// wrong motion sign leaves the history trailing to one side.
int Align(const fs::path &dir) {
  auto S=LoadSnapshot(dir);
  const auto &c=S.at(3),&d=S.at(0),&m=S.at(1),&o=S.at(4);const unsigned W=c.w,H=c.h,OW=o.w,OH=o.h;
  auto is_sky=[&](unsigned x,unsigned y){return d.at(x,y)<1e-6f;};
  std::vector<float> sky;double mx=0,my=0;size_t n=0;
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)if(is_sky(x,y)){sky.push_back(Luma(c,x,y));mx+=m.at(x,y,0);my+=m.at(x,y,1);++n;}
  mx=n?mx/n*OW:0;my=n?my/n*OH:0;   // one frame's sky motion in output pixels
  const double len=std::hypot(mx,my);
  std::cout << "sky motion per frame (output px): (" << mx << ", " << my << ")\n";
  if(len<.5){std::cout << "camera still; alignment not measurable\n";return 0;}
  const double ux=mx/len,uy=my/len;
  std::vector<float> osky;for(unsigned y=0;y<OH;y+=3)for(unsigned x=0;x<OW;x+=3)
    if(is_sky(std::min(W-1,unsigned((x+.5)*W/OW)),std::min(H-1,unsigned((y+.5)*H/OH))))osky.push_back(Luma(o,x,y));
  const float omed=Percentile(osky,.5);
  const auto peaks=Peaks(c,is_sky,2,2.f,std::max(1e-5f,1.5f*Percentile(sky,.5)));
  const int reach=int(std::min(200.0,4*len));std::vector<double> profile(2*reach+1,0);size_t used=0;
  for(const auto &p:peaks) {
    const double ox=(p.x+.5)*OW/W,oy=(p.y+.5)*OH/H;bool ok=true;std::vector<double> line(2*reach+1);
    for(int t=-reach;t<=reach&&ok;++t){const int x=int(ox+t*ux),y=int(oy+t*uy);
      if(x<0||y<0||x>=int(OW)||y>=int(OH)){ok=false;break;}line[t+reach]=std::max(0.f,Luma(o,x,y)-omed);}
    if(!ok)continue;++used;for(size_t i=0;i<line.size();++i)profile[i]+=line[i];
  }
  std::cout << "stars used: " << used << "; mean output excess along +motion direction (offset in output px):\n";
  double before=0,after=0,centre=0;
  for(int t=-reach;t<=reach;++t){const double v=used?profile[t+reach]/used:0;
    if(std::abs(t)<=2)centre+=v;else if(t<0)before+=v;else after+=v;
    if(t%std::max(1,reach/20)==0)std::cout << "  " << t << ": " << v << "\n";}
  std::cout << "energy at |offset|<=2: " << centre << "  behind (-): " << before << "  ahead (+): " << after << "\n";
  return 0;
}
int main(int argc,char **argv) {
  try {
    if(argc==4 && std::string(argv[1])=="--compare")return Compare(argv[2],argv[3]);
    if(argc==3 && std::string(argv[1])=="--align")return Align(argv[2]);
    if(argc!=3)throw std::runtime_error("usage: snapshot_analyze <RR-or-SR folder> <output folder> | --compare <A> <B>");
    const fs::path dir=argv[1],out=argv[2];fs::create_directories(out);
    auto img=LoadSnapshot(dir);
    const auto &depth=img.at(0),&motion=img.at(1),&color=img.at(3),&output=img.at(4);
    const unsigned W=color.w,H=color.h,OW=output.w,OH=output.h;
    // Depth statistics: reverse-Z, so the far plane / sky is the smallest value.
    std::vector<float> dv(depth.data.begin(),depth.data.end());
    size_t zero=0,tiny=0;for(float d:dv){zero+=d==0;tiny+=d<1e-6f;}
    std::cout << "frame " << W << "x" << H << " -> " << OW << "x" << OH << "\n";
    std::cout << "depth min " << *std::min_element(dv.begin(),dv.end()) << " max " << *std::max_element(dv.begin(),dv.end())
      << " p01 " << Percentile(dv,.01) << " p50 " << Percentile(dv,.5) << "\n";
    std::cout << "depth ==0: " << 100.0*zero/dv.size() << "%  <1e-6: " << 100.0*tiny/dv.size() << "%\n";
    auto sky=[&](unsigned x,unsigned y){return depth.at(x,y)<1e-6f;};
    auto sky_out=[&](unsigned x,unsigned y){return sky(std::min(W-1,unsigned((x+.5)*W/OW)),std::min(H-1,unsigned((y+.5)*H/OH)));};
    // Motion statistics in sky and non-sky regions.
    for(int region=0;region<2;++region) {
      size_t n=0,zeros=0,bad=0,big=0;std::vector<float> mag;
      for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
        if(sky(x,y)!=(region==0))continue;++n;const float mx=motion.at(x,y,0),my=motion.at(x,y,1);
        if(!std::isfinite(mx)||!std::isfinite(my)){++bad;continue;}
        if(mx==0&&my==0)++zeros;if(std::abs(mx)>=1||std::abs(my)>=1)++big;
        mag.push_back(std::hypot(mx*W,my*H));
      }
      std::cout << (region?"non-sky":"sky") << " motion: pixels " << n << " zero " << (n?100.0*zeros/n:0) << "% nonfinite " << bad
        << " |v|>=1 " << big << " px-magnitude p50 " << Percentile(mag,.5) << " p99 " << Percentile(mag,.99) << "\n";
    }
    std::vector<float> sky_l;for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)if(sky(x,y))sky_l.push_back(Luma(color,x,y));
    const float sky_median=Percentile(sky_l,.5),sky_p999=Percentile(sky_l,.999);
    std::cout << "sky input luminance p50 " << sky_median << " p99 " << Percentile(sky_l,.99) << " p99.9 " << sky_p999 << " max " << Percentile(sky_l,1) << "\n";
    const float floor_value=std::max(1e-5f,1.5f*sky_median);
    auto in_peaks=Peaks(color,sky,2,2.f,floor_value);
    std::cout << "input color (3) sky peaks (>=2x ring): " << in_peaks.size() << "\n";
    if(img.contains(40)) {
      auto p40=Peaks(img.at(40),sky,2,2.f,floor_value);size_t shared=0;
      for(const auto &p:in_peaks){bool hit=false;for(int dy=-1;dy<=1&&!hit;++dy)for(int dx=-1;dx<=1;++dx){
        const float v=Luma(img.at(40),p.x+dx,p.y+dy);if(v>=.5f*p.value){hit=true;break;}}shared+=hit;}
      std::cout << "color-before-transparency (40) sky peaks: " << p40.size() << "; input peaks also >=50% bright in 40: " << shared << "\n";
    }
    std::vector<float> osky;for(unsigned y=0;y<OH;y+=2)for(unsigned x=0;x<OW;x+=2)if(sky_out(x,y))osky.push_back(Luma(output,x,y));
    const float out_median=Percentile(osky,.5);
    auto out_peaks=Peaks(output,sky_out,3,2.f,std::max(1e-5f,1.5f*out_median));
    std::cout << "output sky luminance p50 " << out_median << " p99.9 " << Percentile(osky,.999) << "\n";
    std::cout << "output (4) sky peaks (>=2x ring): " << out_peaks.size() << "  per 1000 sky px in: "
      << 1000.0*in_peaks.size()/std::max<size_t>(1,sky_l.size()) << " out: " << 1000.0*out_peaks.size()/std::max<size_t>(1,osky.size()*4) << "\n";
    // Survival: for each input peak, best contrast in the corresponding output window.
    size_t survived=0;std::vector<float> ratios;
    for(const auto &p:in_peaks) {
      const int ox=int((p.x+.5)*OW/W),oy=int((p.y+.5)*OH/H);float best=0;int bx=ox,by=oy;
      for(int dy=-3;dy<=3;++dy)for(int dx=-3;dx<=3;++dx){const int x=ox+dx,y=oy+dy;if(x<8||y<8||x+8>=int(OW)||y+8>=int(OH))continue;
        const float v=Luma(output,x,y);if(v>best){best=v;bx=x;by=y;}}
      if(bx<8||by<8||bx+8>=int(OW)||by+8>=int(OH))continue;
      double sum=0;unsigned n=0;for(int dy=-8;dy<=8;++dy)for(int dx=-8;dx<=8;++dx){if(std::max(std::abs(dx),std::abs(dy))<4)continue;sum+=Luma(output,bx+dx,by+dy);++n;}
      const float ring=float(sum/n),contrast=best/std::max(ring,1e-6f),in_contrast=p.value/std::max(p.ring,1e-6f);
      survived+=contrast>=1.5f;ratios.push_back((best-ring)/std::max(p.value-p.ring,1e-9f));
    }
    std::cout << "input peaks surviving in output (>=1.5x ring): " << survived << "/" << in_peaks.size()
      << " (" << (in_peaks.empty()?0:100.0*survived/in_peaks.size()) << "%)  excess-brightness ratio out/in p50 " << Percentile(ratios,.5) << "\n";
    // Previews.
    std::vector<float> all;for(unsigned y=0;y<H;y+=2)for(unsigned x=0;x<W;x+=2)all.push_back(Luma(color,x,y));
    const float white=std::max(1e-4f,Percentile(all,.995));
    ColorPng(out/"input_color.png",color,0,0,W,H,white,2);
    ColorPng(out/"output_color.png",output,0,0,OW,OH,white,4);
    if(img.contains(40))ColorPng(out/"color_before_transparency.png",img.at(40),0,0,W,H,white,2);
    {std::vector<unsigned char> rgb(size_t(W/2)*(H/2)*3);
     for(unsigned y=0;y<H/2;++y)for(unsigned x=0;x<W/2;++x){const unsigned sx=x*2,sy=y*2;auto *p=&rgb[(size_t(y)*(W/2)+x)*3];
       if(sky(sx,sy)){p[0]=0;p[1]=0;p[2]=96;}else{const auto v=Srgb(std::min(1.f,depth.at(sx,sy)*50));p[0]=p[1]=p[2]=v;}
       const float mx=motion.at(sx,sy,0)*W,my=motion.at(sx,sy,1)*H;if(std::isfinite(mx)&&std::isfinite(my)){
         p[0]=(unsigned char)std::clamp(int(p[0])+int(std::abs(mx)*20),0,255);p[1]=(unsigned char)std::clamp(int(p[1])+int(std::abs(my)*20),0,255);}}
     Png(out/"depth_sky_motion.png",W/2,H/2,rgb);}
    // Crop around the densest cluster of input peaks; same region in output.
    if(!in_peaks.empty()) {
      const unsigned cw=160,ch=120;unsigned best_n=0,bx=0,by=0;
      for(unsigned y=0;y+ch<H;y+=20)for(unsigned x=0;x+cw<W;x+=20){unsigned n=0;
        for(const auto &p:in_peaks)n+=p.x>=x&&p.x<x+cw&&p.y>=y&&p.y<y+ch;if(n>best_n){best_n=n;bx=x;by=y;}}
      std::vector<float> crop;for(unsigned y=by;y<by+ch;++y)for(unsigned x=bx;x<bx+cw;++x)crop.push_back(Luma(color,x,y));
      // Night sky is near black; scale so the sky is dark grey and 2x-ring peaks stand out.
      const float cwhite=std::max(1e-5f,20*sky_median);(void)crop;
      ColorPng(out/"crop_input.png",color,bx,by,cw,ch,cwhite,1,4);
      if(img.contains(40))ColorPng(out/"crop_before_transparency.png",img.at(40),bx,by,cw,ch,cwhite,1,4);
      const unsigned ox=unsigned(bx*double(OW)/W),oy=unsigned(by*double(OH)/H),ocw=unsigned(cw*double(OW)/W),och=unsigned(ch*double(OH)/H);
      ColorPng(out/"crop_output.png",output,ox,oy,ocw,och,cwhite,1,2);
      std::cout << "crop at input " << bx << "," << by << " " << cw << "x" << ch << " with " << best_n << " peaks; output " << ox << "," << oy << " " << ocw << "x" << och << "\n";
    }
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
