#pragma once
// Explicit synthetic hardware allowlist; no fallback or inference.
static ComPtr<IDXGIAdapter> SelectAdapter(IDXGIFactory4* factory,bool hardware) {
    ComPtr<IDXGIAdapter> chosen;
    if(!hardware) { Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen)),"WARP adapter"); return chosen; }
    for(UINT index=0;;++index) {
        ComPtr<IDXGIAdapter1> candidate; const auto hr=factory->EnumAdapters1(index,&candidate);
        if(hr==DXGI_ERROR_NOT_FOUND) break;
        Check(hr,"enumerate hardware adapters"); DXGI_ADAPTER_DESC1 d{}; Check(candidate->GetDesc1(&d),"adapter description");
        if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if(d.VendorId!=0x10de || d.DeviceId!=0x2520) continue;
        Require(!chosen,"ambiguous NVIDIA adapter selection"); Check(candidate.As(&chosen),"selected adapter");
    }
    Require(bool(chosen),"required NVIDIA 10de:2520 hardware adapter absent"); return chosen;
}
static void VerifyAdapter(IDXGIAdapter* adapter,ID3D12Device* d12,ID3D11Device* d11,bool hardware) {
    DXGI_ADAPTER_DESC desc{}; Check(adapter->GetDesc(&desc),"selected description");
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> actual11;
    Check(d11->QueryInterface(IID_PPV_ARGS(&dxgi)),"reference DXGI device"); Check(dxgi->GetAdapter(&actual11),"reference adapter");
    DXGI_ADAPTER_DESC desc11{}; Check(actual11->GetDesc(&desc11),"reference description"); const auto luid12=d12->GetAdapterLuid();
    const bool match=desc.AdapterLuid.LowPart==luid12.LowPart && desc.AdapterLuid.HighPart==luid12.HighPart &&
        desc.AdapterLuid.LowPart==desc11.AdapterLuid.LowPart && desc.AdapterLuid.HighPart==desc11.AdapterLuid.HighPart;
    LARGE_INTEGER driver{}; const auto driverStatus=adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver);
    std::cout<<"adapter hardware="<<hardware<<" vendor="<<desc.VendorId<<" device="<<desc.DeviceId
        <<" luid_low="<<desc.AdapterLuid.LowPart<<" luid_high="<<desc.AdapterLuid.HighPart
        <<" same_adapter="<<match<<" driver_query_ok="<<SUCCEEDED(driverStatus)<<" driver="<<driver.QuadPart<<'\n'<<std::flush;
    Require(match,"D3D11/D3D12 adapter LUID mismatch");
    if(hardware) Require(desc.VendorId==0x10de && desc.DeviceId==0x2520,"hardware identity mismatch");
}