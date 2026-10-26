// BF2 menu assets are decoded from the user's disc archive; no game data is shipped here.
#include "pc_settings.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <span>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
void Require(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) throw std::runtime_error("Invalid BF2 theme asset at loader line " + std::to_string(at.line()));
}
uint32_t LE(View b, size_t p, unsigned n = 4) {
  Require(p <= b.size() && n <= b.size() - p);
  uint32_t v = 0; for (unsigned i = 0; i < n; ++i) v |= uint32_t(b[p+i]) << (i*8); return v;
}
uint32_t BE(View b, size_t p) {
  Require(p <= b.size() && 4 <= b.size()-p);
  return uint32_t(b[p])<<24 | uint32_t(b[p+1])<<16 | uint32_t(b[p+2])<<8 | b[p+3];
}
Bytes Read(std::ifstream& f, size_t n) {
  Require(n <= 32*1024*1024); Bytes b(n);
  Require(bool(f.read(reinterpret_cast<char*>(b.data()), n))); return b;
}
// Small bounded RFC1950/RFC1951 decoder. This avoids introducing a DLL dependency
// for the three UI images inside EA's PZFB wrapper. All reads and backreferences
// are checked, and decompression is capped independently of the archive header.
struct Bits {
  View b; size_t pos = 0;
  unsigned Get(unsigned n) {
    Require(pos+n <= b.size()*8); unsigned v=0;
    for(unsigned i=0;i<n;++i,++pos) v |= ((b[pos/8]>>(pos%8))&1)<<i;
    return v;
  }
};
struct Huffman {
  std::array<unsigned,16> count{};
  std::vector<unsigned> symbols;
  explicit Huffman(const std::vector<unsigned>& lengths) {
    for(unsigned len:lengths) { Require(len<=15); ++count[len]; }
    int left=1; for(unsigned i=1;i<=15;++i) {left=left*2-int(count[i]);Require(left>=0);}
    std::array<unsigned,16> offset{};
    for(unsigned i=1;i<15;++i) offset[i+1]=offset[i]+count[i];
    symbols.resize(lengths.size());
    for(unsigned i=0;i<lengths.size();++i) if(lengths[i]) symbols[offset[lengths[i]]++]=i;
  }
  unsigned Decode(Bits& bits) const {
    unsigned code=0,first=0,index=0;
    for(unsigned len=1;len<=15;++len) {
      code |= bits.Get(1);
      if(code>=first && code-first<count[len]) return symbols[index+code-first];
      index+=count[len]; first=(first+count[len])<<1;code<<=1;
    }
    throw std::runtime_error("Invalid BF2 DEFLATE Huffman code");
  }
};
Bytes Inflate(View src) {
  Require(src.size()>=6 && (src[0]&15)==8 && (src[0]>>4)<=7 && !(src[1]&32));
  Require((unsigned(src[0])*256+src[1])%31==0);
  Bits bits{src.subspan(2,src.size()-6)}; Bytes out;
  auto append=[&](uint8_t v){Require(out.size()<32*1024*1024);out.push_back(v);};
  constexpr unsigned lb[]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
  constexpr unsigned le[]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
  constexpr unsigned db[]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
  constexpr unsigned de[]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
  bool last=false;
  while(!last) {
    last=bits.Get(1)!=0; unsigned kind=bits.Get(2); Require(kind!=3);
    if(kind==0) {
      bits.pos=(bits.pos+7)&~size_t(7);unsigned len=bits.Get(16);Require((len^bits.Get(16))==65535);
      for(unsigned i=0;i<len;++i) append(uint8_t(bits.Get(8))); continue;
    }
    std::vector<unsigned> lens,dist;
    if(kind==1) {
      lens.resize(288);for(unsigned i=0;i<288;++i) lens[i]=i<144?8:i<256?9:i<280?7:8;
      dist.assign(32,5);
    } else {
      unsigned nl=bits.Get(5)+257, nd=bits.Get(5)+1, nc=bits.Get(4)+4;
      Require(nl<=286);
      // RFC1951 code-length alphabet order.
      constexpr unsigned cl_order[]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
      std::vector<unsigned> cl(19); for(unsigned i=0;i<nc;++i) cl[cl_order[i]]=bits.Get(3);
      Huffman ch(cl);std::vector<unsigned> all;
      while(all.size()<nl+nd) {
        unsigned s=ch.Decode(bits);
        if(s<16) all.push_back(s);
        else {
          Require(s<=18 && (s!=16 || !all.empty()));
          unsigned count=s==16?bits.Get(2)+3:s==17?bits.Get(3)+3:bits.Get(7)+11;
          unsigned value=s==16?all.back():0; Require(count<=nl+nd-all.size());
          all.insert(all.end(),count,value);
        }
      }
      lens.assign(all.begin(),all.begin()+nl);dist.assign(all.begin()+nl,all.end());
    }
    Require(lens[256]!=0); Huffman lit(lens),distance(dist);
    for(;;) {
      unsigned s=lit.Decode(bits); if(s==256) break;
      if(s<256) {append(uint8_t(s));continue;}
      Require(s<=285);s-=257;unsigned length=lb[s]+bits.Get(le[s]);
      unsigned d=distance.Decode(bits);Require(d<30);unsigned back=db[d]+bits.Get(de[d]);
      Require(back<=out.size());for(unsigned i=0;i<length;++i) append(out[out.size()-back]);
    }
  }
  uint32_t a=1,b=0;for(uint8_t c:out) {a=(a+c)%65521;b=(b+a)%65521;}
  Require((b<<16|a)==BE(src,src.size()-4));return out;
}
size_t Tiled(unsigned x,unsigned y,unsigned width,unsigned log) {
  unsigned aw=(width+31)&~31u;
  unsigned macro=((x>>5)+(y>>5)*(aw>>5))<<(log+7);
  unsigned micro=((x&7)+((y&6)<<2))<<log;
  unsigned off=macro+((micro&~15u)<<1)+(micro&15)+((y&8)<<(3+log))+((y&1)<<4);
  return (((off&~511u)<<3)+((off&448)<<2)+(off&63)+((y&16)<<7)+(((((y&8)>>2)+(x>>3))&3)<<6))>>log;
}
std::array<unsigned,3> RGB565(unsigned c) {return {((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};}
pcs::ThemeImage Texture(View src) {
  Require(src.size()>=32 && !std::memcmp(src.data(),"PAMX",4));
  unsigned w=LE(src,16),h=LE(src,20),format=LE(src,28)&63;
  Require(w && h && w<=2048 && h<=2048 && (format==6 || format==18 || format==20));
  pcs::ThemeImage image;image.width=int(w);image.height=int(h);image.rgba.resize(size_t(w)*h);
  auto pixels=src.subspan(32);
  if(format==6) {
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x)
      image.rgba[y*w+x]=BE(pixels,4*Tiled(x,y,w,2));
    return image;
  }
  bool dxt5=format==20;unsigned bs=dxt5?16:8,bw=(w+3)/4,bh=(h+3)/4;
  for(unsigned y=0;y<bh;++y) for(unsigned x=0;x<bw;++x) {
    size_t offset=Tiled(x,y,bw,dxt5?4:3)*bs; Require(offset+bs<=pixels.size());
    std::array<uint8_t,16> block{};for(unsigned i=0;i<bs;++i) block[i]=pixels[offset+(i^1)];
    View b(block);unsigned cp=dxt5?8:0,c0=LE(b,cp,2),c1=LE(b,cp+2,2),indices=LE(b,cp+4);
    std::array<std::array<unsigned,3>,4> colors{RGB565(c0),RGB565(c1)};
    for(unsigned i=0;i<3;++i) {
      colors[2][i]=c0>c1||dxt5?(2*colors[0][i]+colors[1][i])/3:(colors[0][i]+colors[1][i])/2;
      colors[3][i]=c0>c1||dxt5?(colors[0][i]+2*colors[1][i])/3:0;
    }
    std::array<unsigned,8> alpha{block[0],block[1]};uint64_t alpha_bits=0;
    if(dxt5) {
      for(unsigned i=0;i<6;++i) alpha_bits|=uint64_t(block[2+i])<<(8*i);
      if(alpha[0]>alpha[1]) for(unsigned i=0;i<6;++i) alpha[i+2]=((6-i)*alpha[0]+(i+1)*alpha[1])/7;
      else {for(unsigned i=0;i<4;++i) alpha[i+2]=((4-i)*alpha[0]+(i+1)*alpha[1])/5;alpha[6]=0;alpha[7]=255;}
    }
    for(unsigned k=0;k<16;++k) {
      unsigned px=x*4+k%4,py=y*4+k/4,index=(indices>>(2*k))&3;
      unsigned a=dxt5?alpha[(alpha_bits>>(3*k))&7]:((c0<=c1 && index==3)?0:255);
      auto c=colors[index];if(px<w && py<h) image.rgba[py*w+px]=a<<24|c[0]<<16|c[1]<<8|c[2];
    }
  }
  return image;
}

void LoadTheme(pcs::Theme& theme) {
  try {
    auto root=rex::cvar::GetFlagByName("game_data_root");
    Require(!root.empty());
    std::ifstream f(std::filesystem::u8path(root)/"Data1.viv",std::ios::binary);
    Require(bool(f)); f.seekg(0,std::ios::end);auto file_size=f.tellg();f.seekg(0);
    Bytes header=Read(f,16);Require(!std::memcmp(header.data(),"BIGF",4) || !std::memcmp(header.data(),"BIG4",4));
    uint32_t count=BE(header,8);Require(count<1000000);
    struct Entry {uint32_t offset,size;};Entry font{},atlas{},movie{};
    for(uint32_t i=0;i<count;++i) {
      Bytes e=Read(f,8);std::string name;char c=0;
      do {Require(bool(f.get(c)) && name.size()<1024);if(c) name+=c;} while(c);
      std::replace(name.begin(),name.end(),'/','\\');
      std::transform(name.begin(),name.end(),name.begin(),[](unsigned char v){return char(std::tolower(v));});
      Entry value{BE(e,0),BE(e,4)};
      Require(uint64_t(value.offset)+value.size<=uint64_t(file_size));
      if(name=="border\\menus\\textures\\eafont\\univer24.sfn") font=value;
      if(name=="border\\menus\\textures\\eafont\\univer24.sux") atlas=value;
      if(name=="border\\menus\\apt\\bfmc360_lib.cat") movie=value;
    }
    auto entry=[&](Entry e){Require(e.size>0);f.seekg(e.offset);return Read(f,e.size);};
    Bytes sfn=entry(font);pcs::ThemeFont loaded_font;loaded_font.atlas=Texture(entry(atlas));
    // FntS header +10 stores the glyph count; the following kerning data is not a sentinel.
    unsigned glyph_count=LE(sfn,34,2);Require(glyph_count>100 && 0x98+glyph_count*16<=sfn.size());
    for(size_t p=0x98;p<0x98+glyph_count*16;p+=16) {
      uint16_t code=uint16_t(LE(sfn,p,2));if(!code) break;
      pcs::ThemeGlyph g{};g.code=code;g.width=sfn[p+2];g.height=sfn[p+3];
      g.x=uint16_t(LE(sfn,p+4,2));g.y=uint16_t(LE(sfn,p+6,2));g.offset_x=int8_t(sfn[p+9]);
      g.offset_y=sfn[p+10];g.advance=uint16_t(LE(sfn,p+14,2));
      Require(g.x+g.width<=loaded_font.atlas.width && g.y+g.height<=loaded_font.atlas.height);
      loaded_font.glyphs.push_back(g);
    }
    Require(loaded_font.glyphs.size()>100);loaded_font.line_height=22;
    theme.font=std::move(loaded_font);
    Bytes compressed=entry(movie);Require(compressed.size()>16 && !std::memcmp(compressed.data(),"PZFB",4));
    Bytes contents=Inflate(View(compressed).subspan(16));
    for(size_t p=0;p+8<=contents.size();) {
      unsigned n=LE(contents,p);p+=4;Require(n>0 && n<=256 && p+n+4<=contents.size());
      std::string name(reinterpret_cast<const char*>(contents.data()+p),n);p+=n;
      if(auto nul=name.find('\0');nul!=std::string::npos) name.resize(nul);
      unsigned size=LE(contents,p);p+=4;Require(size<=contents.size()-p);
      pcs::ThemeImage* target=nullptr;
      if(name=="bfmc360_lib17.sux") target=&theme.highlight;
      if(name=="bfmc360_lib34.sux") target=&theme.button_a;
      if(name=="bfmc360_lib33.sux") target=&theme.button_b;
      if(name=="bfmc360_lib10.sux") target=&theme.panel_top;
      if(name=="bfmc360_lib21.sux") target=&theme.panel_bottom;
      if(target) *target=Texture(View(contents).subspan(p,size));
      p+=size;
    }
    REXLOG_INFO("BF2 PC theme: {} Univer glyphs, highlight {}x{}, A {}x{}, B {}x{} loaded from Data1.viv",
                theme.font.glyphs.size(),theme.highlight.width,theme.highlight.height,
                theme.button_a.width,theme.button_a.height,theme.button_b.width,theme.button_b.height);
  } catch(const std::exception& e) {
    REXLOG_WARN("BF2 PC theme: {} (using available assets and procedural fallback)",e.what());
  }
}
} // namespace

const pcs::Theme* PcBf2Theme() {
  static pcs::Theme theme=[] {pcs::Theme t;t.load=LoadTheme;return t;}();
  return &theme;
}




