#include "resident_texture_run.h"
#include "test_support.h"
#include "parity_checks.h"
#include <iomanip>

constexpr UINT W=576, H=320, SW=1920, SH=1080;
constexpr size_t InputCount=size_t(W)*H*3, OutputCount=size_t(W)*H*5;

struct Fixture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    Fixture(ID3D11Device* device, const std::vector<unsigned char>& bytes) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=SW; d.Height=SH;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1; d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        d.Usage=D3D11_USAGE_IMMUTABLE; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{bytes.data(),SW*4,0};
        Check(device->CreateTexture2D(&d,&data,&texture),"combined fixture");
        Check(device->CreateShaderResourceView(texture.Get(),nullptr,&view),"combined fixture view");
    }
};
struct Pair {
    std::array<std::shared_ptr<Fixture>,2> images;
    Pair(ID3D11Device* device, unsigned mode) {
        images[0]=std::make_shared<Fixture>(device,Pixels(SW,SH,0,mode==2));
        images[1]=std::make_shared<Fixture>(device,Pixels(SW,SH,mode==0 ? 0u : 1u,mode==2));
    }
    std::array<GpuProducerBridge::Capture,2> Captures() const {
        return {{{images[0]->view.Get(),images[0]}, {images[1]->view.Get(),images[1]}}};
    }
};
struct InputOracle {
    Commands commands;
    std::array<ComPtr<ID3D12Resource>,2> buffers;
    InputOracle(ID3D12Device* device, GpuProducerBridge& bridge) : commands(device) {
        for(size_t n=0;n<2;++n) {
            buffers[n]=GpuInputPack::Buffer(device,InputCount*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            GpuInputPack::Transition(commands.list.Get(),bridge.Output(n),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands.list->CopyBufferRegion(buffers[n].Get(),0,bridge.Output(n),0,InputCount*4);
            GpuInputPack::Transition(commands.list.Get(),bridge.Output(n),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    void Compare(const std::array<std::vector<float>,2>& expected) {
        for(size_t n=0;n<2;++n) {
            void* p=nullptr; D3D12_RANGE read{0,InputCount*4}, empty{0,0};
            Check(buffers[n]->Map(0,&read,&p),"input oracle map");
            const bool same=std::memcmp(p,expected[n].data(),InputCount*4)==0;
            buffers[n]->Unmap(0,&empty); Require(same,"preprocessing differs");
        }
    }
};
struct FieldOracle {
    Commands commands;
    ComPtr<ID3D12Resource> buffer;
    explicit FieldOracle(ID3D12Device* device) : commands(device) {
        buffer=GpuInputPack::Buffer(device,GpuFlowOutput::Bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void Execute(ID3D12Resource* fields, ID3D12CommandQueue* queue) {
        GpuInputPack::Transition(commands.list.Get(),fields,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands.list->CopyBufferRegion(buffer.Get(),0,fields,0,GpuFlowOutput::Bytes);
        GpuInputPack::Transition(commands.list.Get(),fields,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands.Execute(queue);
    }
    std::vector<float> Read() {
        std::vector<float> values(OutputCount); void* p=nullptr;
        D3D12_RANGE read{0,OutputCount*4},empty{0,0};
        Check(buffer->Map(0,&read,&p),"field oracle map");
        std::memcpy(values.data(),p,OutputCount*4); buffer->Unmap(0,&empty); return values;
    }
};
static std::vector<unsigned char> ReconstructReference(ID3D11Device* device, ID3D11DeviceContext* immediate,
    Pair& pair, const std::vector<float>& fields) {
    RifeReconstructor recon; std::string error;
    Require(recon.Initialize(device,error),error.c_str()); ComPtr<ID3D11ShaderResourceView> view;
    Require(recon.Reconstruct(device,immediate,pair.images[0]->view.Get(),pair.images[1]->view.Get(),fields,W,H,view),"combined reference");
    ConsumerOracle oracle(device,SW,SH); oracle.Execute(immediate,view.Get()); return oracle.Read(immediate);
}
static unsigned ComparePixels(const std::vector<unsigned char>& a,const std::vector<unsigned char>& b) {
    Require(a.size()==size_t(SW)*SH*4 && b.size()==a.size(),"pixel extent"); unsigned maximum=0;
    for(size_t i=0;i<a.size();++i) {
        maximum=(std::max)(maximum,static_cast<unsigned>(std::abs(int(a[i])-int(b[i]))));
        if(i%4==3) Require(a[i]==255 && b[i]==255,"opaque output");
    }
    return maximum;
}
template<class T> static void SaveFloats(const std::string& name,const T& values) {
    std::vector<unsigned char> bytes(values.size()*sizeof(float));
    std::memcpy(bytes.data(),values.data(),bytes.size()); SaveArtifact(name,bytes);
}
struct RunDrain {
    ResidentTextureRun& run;
    ~RunDrain() { try { if(run.Submitted() && !run.Retire(30000)) std::terminate(); } catch(...) { std::terminate(); } }
};
struct CaseOracles {
    InputOracle input; FieldOracle fields; ConsumerOracle draw;
    CaseOracles(ID3D12Device* d12,ID3D11Device* d11,GpuProducerBridge& bridge)
        : input(d12,bridge),fields(d12),draw(d11,SW,SH) {}
};
static void ModelCase(unsigned mode,ID3D11Device* device,ID3D11DeviceContext* immediate,
    const std::shared_ptr<GpuProviderContext>& context,const std::shared_ptr<GpuBoundSession>& session) {
    Pair pair(device,mode); std::array<std::vector<float>,2> expected;
    for(size_t n=0;n<2;++n) {
        RifePreprocessor pre; std::string error;
        Require(pre.Initialize(device,W,H,error),error.c_str());
        std::vector<uint8_t> compact;
        Require(pre.Read(immediate,pair.images[n]->view.Get(),1,compact),"baseline resize/read");
        RifePreprocessor::PackRgb(compact,expected[n]);
    }
    auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    const std::array<int64_t,4> inShape{1,3,H,W},outShape{1,5,H,W};
    std::array<Ort::Value,2> inputs{
        Ort::Value::CreateTensor<float>(memory,expected[0].data(),InputCount,inShape.data(),4),
        Ort::Value::CreateTensor<float>(memory,expected[1].data(),InputCount,inShape.data(),4)};
    std::vector<float> reference(OutputCount);
    auto outputValue=Ort::Value::CreateTensor<float>(memory,reference.data(),OutputCount,outShape.data(),4);
    std::cout<<"baseline_run_started="<<mode<<'\n'<<std::flush;
    try { session->RunForParity({inputs[0],inputs[1]},outputValue,false); }
    catch(...) { Drain(*context); throw; }
    Drain(*context); Require(CompareParity(reference,reference).Passed(),"nonfinite CPU reference");
    auto bridge=std::make_unique<GpuProducerBridge>(context,immediate,pair.Captures(),W,H,session);
    Require(bridge->CpuReadbacks()==0,"bridge readback");
    auto fields=std::make_unique<GpuFlowOutput>(context);
    auto oracles=std::make_shared<CaseOracles>(context->Device(),device,*bridge);
    ResidentTextureRun run(context,session,bridge,fields,immediate,oracles);
    Oracle textureOracle(context->Device(),run.TestOutput()); RunDrain cleanup{run};
    std::cout<<"resident_run_started="<<mode<<'\n'<<std::flush;
    run.SubmitForTest([&](auto* dc,auto* view) { oracles->draw.Execute(dc,view); },
        [&](auto* queue) { oracles->input.commands.Execute(queue); },
        [&](auto* flow,auto* queue) { oracles->fields.Execute(flow,queue); });
    textureOracle.commands.Execute(context->Queue()); Drain(*context);
    Require(run.Retire(30000),"combined retirement");
    oracles->input.Compare(expected);
    const auto actual=oracles->fields.Read(); const auto metrics=CompareParity(actual,reference);
    const auto pixels=textureOracle.Read(),draw=oracles->draw.Read(immediate);
    const auto matchedReference=ReconstructReference(device,immediate,pair,actual);
    const auto endToEndReference=ReconstructReference(device,immediate,pair,reference);
    const auto prefix="case_"+std::to_string(mode);
    SaveFloats(prefix+"_actual.f32",actual); SaveFloats(prefix+"_reference.f32",reference);
    SaveArtifact(prefix+"_candidate.bgra",pixels); SaveArtifact(prefix+"_draw.bgra",draw);
    SaveArtifact(prefix+"_matched_reference.bgra",matchedReference);
    SaveArtifact(prefix+"_end_to_end_reference.bgra",endToEndReference);
    const auto matched=ComparePixels(pixels,matchedReference),combined=ComparePixels(pixels,endToEndReference);
    std::cout<<std::setprecision(17)<<"case="<<mode<<" preprocessing_bit_exact=1 max_scaled="<<metrics.maxScaled
        <<" nonfinite="<<metrics.nonfinite<<" violations="<<metrics.violations<<" reconstruction_max="<<matched
        <<" end_to_end_max="<<combined<<" draw_exact="<<(draw==pixels)<<'\n'<<std::flush;
    Require(metrics.Passed(),"FP16 resident field parity"); Require(draw==pixels,"shared draw parity");
    Require(matched<=1 && combined<=1,"native pixel parity");
}

struct InvalidWriter {
    Commands commands; ComPtr<ID3D12RootSignature> root; ComPtr<ID3D12PipelineState> pipeline;
    explicit InvalidWriter(ID3D12Device* device):commands(device) {
        D3D12_ROOT_PARAMETER p[2]{}; p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[1].Constants.Num32BitValues=2;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=2; rd.pParameters=p;
        ComPtr<ID3DBlob> sig,err,shader;
        Check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err),"invalid root serialize");
        Check(device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root)),"invalid root");
        const char code[]=R"(
RWByteAddressBuffer fields:register(u0);
cbuffer Parameters:register(b0) { uint index; uint bits; };
[numthreads(256,1,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x<921600) fields.Store(p.x*4,p.x==index ? bits : (p.x>=737280 ? 0x3f000000u : 0u));
})";
        Check(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"main","cs_5_0",
            D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&err),"invalid shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get(); pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
        Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"invalid pipeline");
    }
    void Execute(ID3D12Resource* flow,ID3D12CommandQueue* queue,UINT index,UINT bits) {
        commands.list->SetPipelineState(pipeline.Get()); commands.list->SetComputeRootSignature(root.Get());
        commands.list->SetComputeRootUnorderedAccessView(0,flow->GetGPUVirtualAddress());
        const UINT data[]{index,bits}; commands.list->SetComputeRoot32BitConstants(1,2,data,0);
        commands.list->Dispatch(3600,1,1); commands.Execute(queue);
    }
};
struct SyntheticWork {
    std::shared_ptr<GpuProducerBridge> bridge;
    InvalidWriter writer;
    std::shared_ptr<ConsumerOracle> draw;
    GuardedTextureWork guarded;
    SyntheticWork(const std::shared_ptr<GpuProviderContext>& context,ID3D11Device* device,ID3D11DeviceContext* immediate,Pair& pair)
        :bridge(std::make_shared<GpuProducerBridge>(context,immediate,pair.Captures(),W,H)),writer(context->Device()),
         draw(std::make_shared<ConsumerOracle>(device,SW,SH)),guarded(context,*bridge,immediate,draw) {}
};
static void Synthetic(ID3D11Device* device,ID3D11DeviceContext* immediate,const std::shared_ptr<GpuProviderContext>& context) {
    // Every channel's first/last element, infinities and both NaN kinds. Finite
    // signed zero, subnormal and maximum finite must not be falsely rejected.
    const std::array<UINT,8> bits{0x7f800000u,0xff800000u,0x7fc00000u,0x7f800001u,0xffc00000u,0x80000000u,1u,0x7f7fffffu};
    for(unsigned c=0;c<5;++c) for(unsigned edge=0;edge<2;++edge) for(unsigned k=0;k<bits.size();++k) {
        auto work=std::make_shared<std::pair<InvalidWriter,FiniteFields>>(std::piecewise_construct,
            std::forward_as_tuple(context->Device()),std::forward_as_tuple(context));
        GpuFlowOutput output(context); bool finite=false;
        try { output.Submit(work,[&](auto*,auto* flow,auto* queue) {
            work->first.Execute(flow,queue,c*184320+(edge ? 184319u : 0u),bits[k]);
            finite=work->second.CheckFinite(flow,queue,30000);
        }); } catch(...) { if(!output.Wait(30000)) std::terminate(); throw; }
        Require(output.Wait(30000),"finite test retirement"); Require(finite==(k>=5),"finite decision incorrect");
    }
    Pair pair(device,1);
    for(unsigned bad=0;bad<2;++bad) {
        auto work=std::make_shared<SyntheticWork>(context,device,immediate,pair);
        GpuFlowOutput output(context); bool published=false,rejected=false;
        work->bridge->SubmitProducer();
        try { output.Submit(work,[&](auto*,auto* flow,auto* queue) {
            work->bridge->SubmitConsumer([&](const auto&,const auto&,auto*) {
                work->writer.Execute(flow,queue,921599,bad ? 0x7fc00000u : 0x3f000000u);
                work->guarded.Consume(flow,queue,[&](auto* dc,auto* view) { published=true; work->draw->Execute(dc,view); });
            });
        }); } catch(const std::domain_error&) { rejected=true; }
        catch(...) { Drain(*context); output.Wait(30000); work->bridge->Retire(30000); throw; }
        Require(output.Wait(30000),"publication output retirement");
        Require(work->bridge->Retire(30000)==GpuSubmissionOwner::Retirement::Complete,"publication input retirement");
        Require(published==!bad && rejected==bool(bad),"publication policy");
        if(!bad) {
            const auto actual=work->draw->Read(immediate);
            const auto expected=ReconstructReference(device,immediate,pair,Fields(2));
            Require(ComparePixels(actual,expected)<=1,"owned source pair reconstruction mismatch");
        }
    }
    std::cout<<"PASS finite_cases=80 publication_cases=2 source_pair_snapshot=1 model_runs=0\n"<<std::flush;
}

int wmain(int argc,wchar_t** argv) {
    try {
        const bool warp=argc==2 && std::wstring(argv[1])==L"--warp-no-inference";
        const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware-no-inference";
        const bool model=argc==4 && std::wstring(argv[1])==L"--execute-fp16-combined";
        Require(warp || hardware || model,"explicit bounded gate mode required");
        std::vector<unsigned char> modelBytes;
        if(model) {
            const std::wstring suppliedHash=argv[3];
            Require(suppliedHash.size()==64 && suppliedHash.find_first_not_of(L"0123456789abcdef")==std::wstring::npos,
                "explicit lowercase SHA256 required");
            std::string expectedHash;
            expectedHash.reserve(suppliedHash.size());
            for(const auto character : suppliedHash) expectedHash.push_back(static_cast<char>(character));
            const auto bytes=std::filesystem::file_size(argv[2]);
            Require(bytes>0 && bytes<=64ull*1024*1024,"bounded model size required");
            modelBytes=LoadPinned(argv[2],static_cast<size_t>(bytes),expectedHash.c_str());
        }
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        auto adapter=SelectAdapter(factory.Get(),!warp);
        ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> immediate;
        Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&immediate),"D3D11 device");
        VerifyAdapter(adapter.Get(),device.Get(),d11.Get(),!warp);
        auto context=std::make_shared<GpuProviderContext>(device.Get(),queue.Get());
        if(!model) { Synthetic(d11.Get(),immediate.Get(),context); return 0; }
        std::shared_ptr<GpuBoundSession> session;
        try { session=GpuBoundSession::Create(context,modelBytes.data(),modelBytes.size()); }
        catch(...) { Drain(*context); throw; }
        Drain(*context); session->ValidateFlowContract();
        std::cout<<"session_created=1 external_fp32_contract=1 model_hash_verified=1\n"<<std::flush;
        for(unsigned mode=0;mode<3;++mode) ModelCase(mode,d11.Get(),immediate.Get(),context,session);
        Drain(*context);
        std::cout<<"PASS combined_cases=3 model_runs=6 native=1920x1080 status_bytes=4 performance_validated=0\n"<<std::flush;
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n'; return 1; }
}