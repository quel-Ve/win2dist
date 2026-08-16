/**
 * Route E: D3D11 Desktop Duplication + CPU box blur + Dual-Hide
 * ==============================================================
 * GPU capture (DDA) → CPU box blur → UpdateLayeredWindow.
 * DDA is GPU-accelerated; box blur is simple, verified, and avoids
 * Direct2D interop issues with MinGW.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

constexpr int BLUR_RADIUS = 5;
constexpr int BLUR_PASSES = 3;
constexpr int POLL_INTERVAL_MS = 33;
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

static ComPtr<ID3D11Device>        g_device;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<IDXGIOutputDuplication> g_dupl;
static HWND g_target = nullptr, g_backdrop = nullptr;

struct BGRA { BYTE b,g,r,a; };

void fatal(const wchar_t* msg, HRESULT hr = S_OK) {
    wchar_t buf[512];
    swprintf(buf,512,FAILED(hr)?L"[x] %s (0x%08X)":L"[x] %s",msg,hr);
    MessageBoxW(nullptr,buf,L"BlurBackdrop Error",MB_ICONERROR);
    ExitProcess(1);
}

void init_gpu() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)factory.GetAddressOf());
    if(FAILED(hr)) fatal(L"CreateDXGIFactory1 failed",hr);
    for(UINT aidx=0;;aidx++){
        ComPtr<IDXGIAdapter1> adapter;
        hr=factory->EnumAdapters1(aidx,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND) break;
        if(FAILED(hr)) continue;
        DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
        wprintf(L"[*] GPU %u: %s (VRAM %llu MB)\n",aidx,desc.Description,
                desc.DedicatedVideoMemory/1024/1024); fflush(stdout);
        ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL fl;
        hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,
                             D3D11_SDK_VERSION,&dev,&fl,&ctx);
        if(FAILED(hr))continue;
        ComPtr<IDXGIOutput> output;
        for(UINT oidx=0;adapter->EnumOutputs(oidx,&output)!=DXGI_ERROR_NOT_FOUND;oidx++){
            ComPtr<IDXGIOutput1> out1;
            if(FAILED(output.As(&out1))){output.Reset();continue;}
            ComPtr<IDXGIOutputDuplication> dupl;
            hr=out1->DuplicateOutput(dev.Get(),&dupl);
            if(SUCCEEDED(hr)){
                DXGI_OUTPUT_DESC od; output->GetDesc(&od);
                wprintf(L"    [*] DDA OK: %s\n",od.DeviceName); fflush(stdout);
                g_device=dev; g_ctx=ctx; g_dupl=dupl;
                return;
            }
            output.Reset();
        }
    }
    fatal(L"No GPU supports Desktop Duplication.");
}

// DDA capture → CPU BGRA pixels
bool capture_gpu(int x,int y,int w,int h,std::vector<BYTE>& out){
    g_dupl->ReleaseFrame();
    ComPtr<IDXGIResource> frameRes; DXGI_OUTDUPL_FRAME_INFO fi={};
    HRESULT hr=g_dupl->AcquireNextFrame(16,&fi,&frameRes);
    if(FAILED(hr)) return false;
    ComPtr<ID3D11Texture2D> desktop; frameRes.As(&desktop);
    D3D11_TEXTURE2D_DESC sd={};
    sd.Width=w; sd.Height=h; sd.MipLevels=1; sd.ArraySize=1;
    sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count=1;
    sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr=g_device->CreateTexture2D(&sd,nullptr,&staging);
    if(FAILED(hr)){g_dupl->ReleaseFrame();return false;}
    D3D11_BOX box={(UINT)x,(UINT)y,0,(UINT)(x+w),(UINT)(y+h),1};
    g_ctx->CopySubresourceRegion(staging.Get(),0,0,0,0,desktop.Get(),0,&box);
    g_dupl->ReleaseFrame();
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr=g_ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
    if(FAILED(hr)) return false;
    out.resize(w*h*4);
    for(int row=0;row<h;row++)
        memcpy(out.data()+row*w*4,(BYTE*)mapped.pData+row*mapped.RowPitch,w*4);
    g_ctx->Unmap(staging.Get(),0);
    for(int i=3;i<(int)out.size();i+=4)out[i]=255;
    return true;
}

// CPU box blur (verified)
void blur_h(std::vector<BGRA>& buf,int w,int h,int r){
    std::vector<BGRA> tmp(buf.size());
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        int rs=0,gs=0,bs=0,n=0;
        for(int k=-r;k<=r;k++){int sx=x+k;if(sx<0||sx>=w)continue;
            auto&p=buf[y*w+sx];rs+=p.r;gs+=p.g;bs+=p.b;n++;}
        auto&d=tmp[y*w+x];d.r=(BYTE)(rs/n);d.g=(BYTE)(gs/n);d.b=(BYTE)(bs/n);d.a=255;
    }
    buf.swap(tmp);
}
void blur_v(std::vector<BGRA>& buf,int w,int h,int r){
    std::vector<BGRA> tmp(buf.size());
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        int rs=0,gs=0,bs=0,n=0;
        for(int k=-r;k<=r;k++){int sy=y+k;if(sy<0||sy>=h)continue;
            auto&p=buf[sy*w+x];rs+=p.r;gs+=p.g;bs+=p.b;n++;}
        auto&d=tmp[y*w+x];d.r=(BYTE)(rs/n);d.g=(BYTE)(gs/n);d.b=(BYTE)(bs/n);d.a=255;
    }
    buf.swap(tmp);
}
void blur(std::vector<BGRA>& buf,int w,int h){
    for(int i=0;i<BLUR_PASSES;i++){blur_h(buf,w,h,BLUR_RADIUS);blur_v(buf,w,h,BLUR_RADIUS);}
}

void render_layered(const std::vector<BYTE>& buf,int w,int h){
    HDC scr=GetDC(nullptr),mem=CreateCompatibleDC(scr);
    BITMAPINFO bi={};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void*bits=nullptr;HBITMAP bmp=CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(bits&&!buf.empty())memcpy(bits,buf.data(),buf.size());
    HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
    BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    SIZE sz={w,h};POINT pt={0,0};
    UpdateLayeredWindow(g_backdrop,scr,nullptr,&sz,mem,&pt,0,&blend,ULW_ALPHA);
    SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);ReleaseDC(nullptr,scr);
}

RECT frame(HWND hwnd){
    RECT r={};GetWindowRect(hwnd,&r);
    HMODULE dwm=LoadLibraryW(L"dwmapi.dll");
    if(dwm){auto fn=(HRESULT(WINAPI*)(HWND,DWORD,PVOID,DWORD))GetProcAddress(dwm,"DwmGetWindowAttribute");
        if(fn){RECT f={};if(SUCCEEDED(fn(hwnd,9,&f,sizeof(f)))&&f.right>f.left)r=f;}FreeLibrary(dwm);}
    return r;
}

void tick(){
    if(!IsWindow(g_target)||!IsWindow(g_backdrop))return;
    RECT r=frame(g_target);int w=r.right-r.left,h=r.bottom-r.top;
    if(w<=0||h<=0)return;

    static RECT lastRect={}; static int settleCount=0;
    bool moved = (r.left!=lastRect.left||r.top!=lastRect.top||
                  r.right!=lastRect.right||r.bottom!=lastRect.bottom);
    lastRect=r;

    // Reposition backdrop: g_target precedes g_backdrop (target ABOVE backdrop)
    SetWindowPos(g_backdrop,g_target,r.left,r.top,w,h,
        SWP_NOACTIVATE|SWP_NOREDRAW);

    if(moved){ settleCount=0; return; } // Moving: just reposition, don't capture
    settleCount++;
    if(settleCount<3) return; // Wait 3 stable frames (~100ms) before capture

    // Stationary: dual-hide → capture clean background → show backdrop first
    ShowWindow(g_backdrop,SW_HIDE);
    ShowWindow(g_target,SW_HIDE);
    DwmFlush();
    std::vector<BYTE> buf; bool ok=capture_gpu(r.left,r.top,w,h,buf);
    // Render the captured+blurred frame BEFORE showing windows
    if(ok&&!buf.empty()){
        std::vector<BGRA> bgra(w*h);
        memcpy(bgra.data(),buf.data(),w*h*sizeof(BGRA));
        blur(bgra,w,h);
        memcpy(buf.data(),bgra.data(),w*h*sizeof(BGRA));
        render_layered(buf,w,h);
    }
    ShowWindow(g_backdrop,SW_SHOWNOACTIVATE);
    ShowWindow(g_target,SW_SHOW);

    // Re-affirm z-order: g_target precedes g_backdrop (target ABOVE backdrop)
    SetWindowPos(g_backdrop,g_target,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    settleCount=0; // reset after capture — wait for next change
}

HWND make_backdrop(){
    const wchar_t* CN=L"BlurBackdropClass";
    WNDCLASSW wc={};wc.lpfnWndProc=DefWindowProc;
    wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName=CN;
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        CN,L"BlurBackdrop",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
}

int main(int argc,char*argv[]){
    if(argc<2){fprintf(stderr,"Usage: blur_backdrop.exe <hwnd_hex>\n");return 1;}
    g_target=(HWND)(ULONG_PTR)strtoull(argv[1],nullptr,16);
    if(!IsWindow(g_target)){fprintf(stderr,"[x] Invalid HWND\n");return 1;}
    init_gpu();
    g_backdrop=make_backdrop();ShowWindow(g_backdrop,SW_SHOWNOACTIVATE);
    wchar_t title[256];GetWindowTextW(g_target,title,256);
    wprintf(L"[*] Target: %s\n",title);fflush(stdout);
    wprintf(L"[*] Pipeline: DDA capture -> CPU box blur %dpx x%d -> render\n",BLUR_RADIUS,BLUR_PASSES);
    wprintf(L"[*] Ctrl+C to stop\n");fflush(stdout);
    tick();
    MSG msg;bool run=true;
    while(run){while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
        if(msg.message==WM_QUIT){run=false;break;}TranslateMessage(&msg);DispatchMessage(&msg);}
        if(!run)break;tick();Sleep(POLL_INTERVAL_MS);}
    DestroyWindow(g_backdrop);
    return 0;
}
