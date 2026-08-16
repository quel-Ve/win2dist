/**
 * Crisp Overlay — text-preserving frosted glass (v2.8)
 * =====================================================
 * The DWM acrylic route (acrylic_overlay.exe) makes the TARGET window
 * semi-transparent: at low opacity the app's text faints (layered windows
 * also lose ClearType subpixel rendering).
 *
 * Crisp mode instead keeps the target at 100% opacity — the app renders
 * natively, text stays razor sharp — and composites a blurred snapshot of
 * the desktop background ON TOP of the target at (1 - opacity) alpha.
 * Result: "1.0 target window + x% background" instead of "x% target".
 *
 * Pipeline (reuses Route E infrastructure):
 *   DDA capture (desktop duplication) -> CPU box blur -> black-level tint
 *   -> UpdateLayeredWindow at wash alpha. Overlay sits ABOVE the target,
 *   fully click-through (HTTRANSPARENT).
 *
 * Trade-off vs DWM acrylic: re-capture requires hiding both windows for one
 * frame (dual-hide), so one flicker frame whenever the window MOVES.
 * Stationary windows never re-capture (no flicker, but background changes
 * behind a stationary window are picked up only on next move).
 *
 * Usage: crisp_overlay.exe <hwnd_hex> <wash_alpha 0-255> <tint_black_0-100>
 *   wash_alpha: strength of background wash (255-alpha of target opacity).
 *   tint: black level 0-100; 0 = pure blur, no tint.
 */

#ifndef UNICODE
#define UNICODE
#endif
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
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

static ComPtr<ID3D11Device>        g_device;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<IDXGIOutputDuplication> g_dupl;
static HWND g_target = nullptr, g_overlay = nullptr;
static LONG g_outX = 0, g_outY = 0;      // duplicated output origin (virtual screen coords)
static DWORD g_targetPid = 0;            // HWND-recycling guard
static BYTE g_washAlpha = 64;            // 0-255: background wash strength
static int g_tintPct = 0;                // 0-100 black level
static bool g_running = true;
static bool g_overlayHidden = false;
static HWINEVENTHOOK g_hookFg = nullptr, g_hookObj = nullptr;

struct BGRA { BYTE b,g,r,a; };

void fatal(const wchar_t* msg, HRESULT hr = S_OK) {
    wchar_t buf[512];
    swprintf(buf,512,FAILED(hr)?L"[x] %s (0x%08X)":L"[x] %s",msg,hr);
    MessageBoxW(nullptr,buf,L"CrispOverlay Error",MB_ICONERROR);
    ExitProcess(1);
}

// ---- DDA capture (from Route E, plus output-origin offset) ----
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
        wprintf(L"[*] GPU %u: %s\n",aidx,desc.Description); fflush(stdout);
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
                g_outX = od.DesktopCoordinates.left;
                g_outY = od.DesktopCoordinates.top;
                wprintf(L"    [*] DDA OK: %s (origin %ld,%ld)\n",od.DeviceName,g_outX,g_outY);
                fflush(stdout);
                g_device=dev; g_ctx=ctx; g_dupl=dupl;
                return;
            }
            output.Reset();
        }
    }
    fatal(L"No GPU supports Desktop Duplication.");
}

// Capture a region of the duplicated output into CPU BGRA pixels
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

// ---- CPU box blur (verified in Route E) ----
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

// Black-level tint on the blurred layer (0 = pure blur)
void apply_tint(std::vector<BGRA>& buf,int tintPct){
    if(tintPct<=0) return;
    unsigned mul = (unsigned)(100 - (tintPct>100?100:tintPct));
    for(auto& p : buf){ p.r=(BYTE)(p.r*mul/100); p.g=(BYTE)(p.g*mul/100); p.b=(BYTE)(p.b*mul/100); }
}

void render_layered(const std::vector<BYTE>& buf,int w,int h,BYTE alpha){
    HDC scr=GetDC(nullptr),mem=CreateCompatibleDC(scr);
    BITMAPINFO bi={};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void*bits=nullptr;HBITMAP bmp=CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(bits&&!buf.empty())memcpy(bits,buf.data(),buf.size());
    HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
    BLENDFUNCTION blend={AC_SRC_OVER,0,alpha,AC_SRC_ALPHA};
    SIZE sz={w,h};POINT pt={0,0};
    UpdateLayeredWindow(g_overlay,scr,nullptr,&sz,mem,&pt,0,&blend,ULW_ALPHA);
    SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);ReleaseDC(nullptr,scr);
}

RECT frame(HWND hwnd){
    RECT r={};GetWindowRect(hwnd,&r);
    HMODULE dwm=LoadLibraryW(L"dwmapi.dll");
    if(dwm){auto fn=(HRESULT(WINAPI*)(HWND,DWORD,PVOID,DWORD))GetProcAddress(dwm,"DwmGetWindowAttribute");
        if(fn){RECT f={};if(SUCCEEDED(fn(hwnd,9,&f,sizeof(f)))&&f.right>f.left)r=f;}FreeLibrary(dwm);}
    return r;
}

// Place overlay directly ABOVE target: below the window that currently
// precedes the target in z-order (GW_HWNDPREV). Never moves the target.
void reanchor(){
    HWND above = GetWindow(g_target, GW_HWNDPREV);
    SetWindowPos(g_overlay, above ? above : HWND_TOP, 0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}

void hide_overlay(){ if(g_overlayHidden)return; g_overlayHidden=true;
    if(g_overlay&&IsWindow(g_overlay))ShowWindow(g_overlay,SW_HIDE); }
void show_overlay(){ if(!g_overlayHidden)return; g_overlayHidden=false;
    if(g_overlay&&IsWindow(g_overlay)){ ShowWindow(g_overlay,SW_SHOWNOACTIVATE); reanchor(); } }

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                           LONG, LONG, DWORD, DWORD) {
    if(!g_overlay) return;
    if(event==EVENT_SYSTEM_FOREGROUND){
        // Any activation: keep overlay just above target (idempotent, cheap)
        if(IsWindow(g_target)&&IsWindowVisible(g_target)) reanchor();
    } else if(hwnd==g_target){
        switch(event){
        case EVENT_OBJECT_DESTROY: g_running=false; break;
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_CLOAKED: hide_overlay(); break;
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_UNCLOAKED:
            if(IsWindow(g_target)) show_overlay();
            break;
        }
    }
}

void tick(){
    if(!IsWindow(g_target)||!IsWindow(g_overlay))return;
    DWORD pid=0; GetWindowThreadProcessId(g_target,&pid);
    if(pid!=g_targetPid){ wprintf(L"[!] Target HWND recycled, exiting\n"); g_running=false; return; }
    if(!IsWindowVisible(g_target)){ hide_overlay(); return; }
    show_overlay();

    RECT r=frame(g_target); int w=r.right-r.left,h=r.bottom-r.top;
    if(w<=0||h<=0)return;

    static RECT lastRect={}; static int settleCount=0;
    bool moved = (r.left!=lastRect.left||r.top!=lastRect.top||
                  r.right!=lastRect.right||r.bottom!=lastRect.bottom);
    lastRect=r;

    reanchor();                     // keep overlay just above target
    SetWindowPos(g_overlay,nullptr,r.left,r.top,w,h,
        SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOZORDER);

    if(moved){ settleCount=0; return; } // Moving: reposition only, no capture
    settleCount++;
    if(settleCount<3) return;           // Wait 3 stable frames (~100ms) before capture

    // Stationary: dual-hide -> capture clean background -> render -> show
    ShowWindow(g_overlay,SW_HIDE);
    ShowWindow(g_target,SW_HIDE);
    DwmFlush();
    std::vector<BYTE> buf; bool ok=capture_gpu(r.left-g_outX,r.top-g_outY,w,h,buf);
    if(ok&&!buf.empty()){
        std::vector<BGRA> bgra(w*h);
        memcpy(bgra.data(),buf.data(),w*h*sizeof(BGRA));
        blur(bgra,w,h);
        apply_tint(bgra,g_tintPct);
        memcpy(buf.data(),bgra.data(),w*h*sizeof(BGRA));
        render_layered(buf,w,h,g_washAlpha);
    }
    ShowWindow(g_target,SW_SHOW);
    ShowWindow(g_overlay,SW_SHOWNOACTIVATE);
    reanchor();
    settleCount=0; // reset after capture — wait for next change
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        // Click-through is MANDATORY here: overlay sits ABOVE the target,
        // so all mouse input must pass through to the window underneath.
        return HTTRANSPARENT;
    case WM_CLOSE:
        g_running = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc,char*argv[]){
    if(argc<3){fprintf(stderr,"Usage: crisp_overlay.exe <hwnd_hex> <wash_alpha 0-255> [tint_black 0-100]\n");return 1;}
    g_target=(HWND)(ULONG_PTR)strtoull(argv[1],nullptr,16);
    if(!IsWindow(g_target)){fprintf(stderr,"[x] Invalid HWND\n");return 1;}
    GetWindowThreadProcessId(g_target,&g_targetPid);
    g_washAlpha=(BYTE)atoi(argv[2]);
    if(argc>=4){g_tintPct=atoi(argv[3]);if(g_tintPct<0)g_tintPct=0;if(g_tintPct>100)g_tintPct=100;}

    init_gpu();

    const wchar_t* CN=L"CrispOverlayClass";
    WNDCLASSW wc={};wc.lpfnWndProc=OverlayWndProc;
    wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName=CN;
    RegisterClassW(&wc);
    g_overlay=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        CN,L"CrispOverlay",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    if(!g_overlay){fprintf(stderr,"[x] Failed to create overlay (err=%lu)\n",GetLastError());return 1;}

    wchar_t title[256];GetWindowTextW(g_target,title,256);
    wprintf(L"[*] Target: %s\n",title);fflush(stdout);
    wprintf(L"[*] Pipeline: DDA capture -> CPU box blur %dpx x%d -> tint %d%% -> wash alpha %u\n",
            BLUR_RADIUS,BLUR_PASSES,g_tintPct,g_washAlpha);fflush(stdout);

    g_hookFg=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,
        nullptr,WinEventProc,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    g_hookObj=SetWinEventHook(EVENT_OBJECT_HIDE,EVENT_OBJECT_UNCLOAKED,
        nullptr,WinEventProc,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);

    ShowWindow(g_overlay,SW_SHOWNOACTIVATE);
    tick();  // first capture immediately
    MSG msg;
    while(g_running){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
        if(msg.message==WM_QUIT){g_running=false;break;}
        TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(!g_running)break;
        tick();Sleep(POLL_INTERVAL_MS);}

    if(g_hookFg)UnhookWinEvent(g_hookFg);
    if(g_hookObj)UnhookWinEvent(g_hookObj);
    DestroyWindow(g_overlay);
    wprintf(L"[*] Overlay destroyed\n");
    return 0;
}
