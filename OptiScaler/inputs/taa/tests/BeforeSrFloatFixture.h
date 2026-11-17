// Actual TAA-hook float fixture helpers. These readbacks belong only to the host.
#pragma once
#include <optional>
#include <vector>
#include <limits>
namespace before_sr_float {
struct Case { const wchar_t* name; DXGI_FORMAT format; };
inline std::optional<Case> Find(const std::wstring& name) {
    for(const auto& item:std::array<Case,3>{{{L"before-sr-r11",DXGI_FORMAT_R11G11B10_FLOAT},
        {L"before-sr-fp16",DXGI_FORMAT_R16G16B16A16_FLOAT},{L"before-sr-f32",DXGI_FORMAT_R32G32B32A32_FLOAT}}})
        if(name==item.name)return item;
    return {};
}
inline unsigned Stride(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    default:throw std::runtime_error("unsupported fixture readback format");
    }
}
inline float UnsignedFloat(unsigned bits,unsigned mantissa) {
    const unsigned exponent=bits>>mantissa,fraction=bits&((1u<<mantissa)-1);
    if(exponent==31)return fraction?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity();
    return exponent?std::ldexp(1.0f+float(fraction)/float(1u<<mantissa),int(exponent)-15):
        std::ldexp(float(fraction),-14-int(mantissa));
}
inline std::array<float,4> Decode(const unsigned char* bytes,DXGI_FORMAT format) {
    std::array<float,4> value{0,0,0,1};
    if(format==DXGI_FORMAT_R8G8B8A8_UNORM)for(unsigned c=0;c<4;++c)value[c]=float(bytes[c])/255;
    else if(format==DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::array<unsigned short,4> raw{};std::memcpy(raw.data(),bytes,8);
        for(unsigned c=0;c<4;++c)value[c]=Half(raw[c]);
    } else if(format==DXGI_FORMAT_R32G32B32A32_FLOAT)std::memcpy(value.data(),bytes,16);
    else if(format==DXGI_FORMAT_R11G11B10_FLOAT) {
        unsigned raw=0;std::memcpy(&raw,bytes,4);
        value[0]=UnsignedFloat(raw&2047,6);value[1]=UnsignedFloat((raw>>11)&2047,6);value[2]=UnsignedFloat(raw>>22,5);
    } else throw std::runtime_error("unsupported fixture pixel decode");
    return value;
}
struct Pixels {
    unsigned width=0,height=0,stride=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    std::vector<unsigned char> bytes;
    std::array<float,4> At(unsigned x,unsigned y) const {return Decode(bytes.data()+(size_t(y)*width+x)*stride,format);}
    bool Interior(const std::array<float,4>& expected,float tolerance) const {
        for(unsigned y=8;y+8<height;++y)for(unsigned x=8;x+8<width;++x) {
            const auto pixel=At(x,y);
            for(unsigned c=0;c<3;++c)if(!std::isfinite(pixel[c])||std::abs(pixel[c]-expected[c])>tolerance)return false;
        }
        return true;
    }
    bool GreenHistory() const {
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto p=At(x,y);if(p!=std::array<float,4>{0,1,0,1})return false;
        }
        return true;
    }
};
inline Pixels Read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    Pixels pixels{desc.Width,desc.Height,Stride(desc.Format),desc.Format,{}};
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&desc,nullptr,&staging));context->CopyResource(staging.Get(),source);
    D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    const size_t row=size_t(pixels.width)*pixels.stride;pixels.bytes.resize(row*pixels.height);
    for(unsigned y=0;y<pixels.height;++y)std::memcpy(pixels.bytes.data()+y*row,static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch,row);
    context->Unmap(staging.Get(),0);return pixels;
}
inline std::array<float,4> Source(unsigned phase) {
    return std::array<std::array<float,4>,3>{{{2,1.5f,.5f,1},{1.5f,2.25f,.75f,1},{2.5f,.75f,1.25f,1}}}.at(phase);
}
inline std::array<float,4> Expected(const Pixels& source) {
    auto value=source.At(source.width/2,source.height/2);
    for(unsigned c=0;c<3;++c)value[c]*=.25f;value[0]+=.125f;return value;
}
} // namespace before_sr_float
