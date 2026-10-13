#define NOMINMAX
#include "../src/rife_reconstructor.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>

using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::vector<float> Load(const std::string& path, size_t count) {
    std::vector<float> data(count);
    std::ifstream file(path, std::ios::binary);
    Check(bool(file.read(reinterpret_cast<char*>(data.data()), count*sizeof(float))), "fixture read");
    return data;
}
int main(int argc, char** argv) {
    try {
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device,nullptr,&context)), "WARP creation");
        RifeReconstructor warp; std::string error;
        Check(warp.Initialize(device.Get(),error), error.c_str());
        auto test = [&](UINT w, UINT h, UINT mw, UINT mh, const std::vector<float>& a,
            const std::vector<float>& b, const std::vector<float>& fields, const std::vector<float>& expected) {
            auto image = [&](const std::vector<float>& planar) {
                std::vector<float> rgba(static_cast<size_t>(w)*h*4,1);
                const size_t p=static_cast<size_t>(w)*h;
                for (size_t i=0;i<p;++i) for(size_t c=0;c<3;++c) rgba[4*i+c]=planar[c*p+i];
                D3D11_TEXTURE2D_DESC d={}; d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
                d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA s={rgba.data(),w*16,0};ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11ShaderResourceView> v;
                Check(SUCCEEDED(device->CreateTexture2D(&d,&s,&t)),"input texture");
                Check(SUCCEEDED(device->CreateShaderResourceView(t.Get(),nullptr,&v)),"input view");return v;
            };
            auto av=image(a),bv=image(b);ComPtr<ID3D11ShaderResourceView> out;
            Check(warp.Reconstruct(device.Get(),context.Get(),av.Get(),bv.Get(),fields,mw,mh,out),"reconstruction");
            ComPtr<ID3D11Resource> resource;out->GetResource(&resource);ComPtr<ID3D11Texture2D> tex;resource.As(&tex);
            D3D11_TEXTURE2D_DESC d={};tex->GetDesc(&d);Check(d.Width==w && d.Height==h,"output dimensions");
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"staging");
            context->CopyResource(staging.Get(),tex.Get());D3D11_MAPPED_SUBRESOURCE map={};
            Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"map");
            float maxError=0; const size_t p=static_cast<size_t>(w)*h;
            for(UINT y=0;y<h;++y) for(UINT x=0;x<w;++x) for(UINT c=0;c<3;++c) {
                float actual=static_cast<const unsigned char*>(map.pData)[y*map.RowPitch+x*4+2-c]/255.f;
                maxError=std::max(maxError,std::abs(actual-expected[c*p+y*w+x]));
            }
            context->Unmap(staging.Get(),0);
            warp.PollGpuTiming(context.Get());
            const auto& timing = warp.LastTiming();
            Check(timing.packMs >= 0 && timing.uploadMs >= 0 && timing.allocationMs >= 0 &&
                timing.drawMs >= 0, "nonnegative CPU reconstruction timings");
            std::cout << "shader max_abs_error=" << maxError << '\n';
            Check(maxError<0.012f,"shader/reference mismatch");
        };
        // Known translation, border clamping, mask endpoints, and 2x flow scaling.
        for(UINT scale : {1u,2u}) for(float mask : {0.f,0.25f,1.f}) {
            UINT w=16*scale,h=8*scale;size_t p=w*h,mp=16*8;
            std::vector<float> a(p*3),b(p*3),f(mp*5),expected(p*3);
            for(size_t i=0;i<mp;++i) { f[i]=1; f[mp+i]=-1; f[mp*2+i]=-1;f[mp*3+i]=1;f[mp*4+i]=mask; }
            for(UINT y=0;y<h;++y) for(UINT x=0;x<w;++x) for(UINT c=0;c<3;++c) {
                a[c*p+y*w+x]=(float(x)/w+float(y)/h)*0.5f;
                b[c*p+y*w+x]=(float(x)/w+float(y)/h)*0.25f;
                float ax=float(std::min(x+scale,w-1))/w, ay=float(y>=scale?y-scale:0)/h;
                float bx=float(x>=scale?x-scale:0)/w, by=float(std::min(y+scale,h-1))/h;
                expected[c*p+y*w+x]=mask*(ax+ay)*0.5f+(1-mask)*(bx+by)*0.25f;
            }
            test(w,h,16,8,a,b,f,expected);
        }
        if(argc==2) {
            const size_t p=576*320;std::string dir=argv[1];
            test(576,320,576,320,Load(dir+"/a.bin",p*3),Load(dir+"/b.bin",p*3),
                Load(dir+"/fields.bin",p*5),Load(dir+"/expected.bin",p*3));
        }
        std::cout << "Reconstruction tests passed\n"; return 0;
    } catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}
}