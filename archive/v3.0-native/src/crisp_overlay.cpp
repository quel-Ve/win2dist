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
#include <tlhelp32.h>   // watch_parent_exit: 父进程看护
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

constexpr int BLUR_PASSES = 3;
constexpr int POLL_INTERVAL_MS = 33;
constexpr int FOCUS_CAPTURE_HOLD_MS = 1500;
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
static int g_radius = 15;            // 1-120 effective blur radius px
static int g_circlePx = 0;           // 0 = mouse circle off
static int g_boostX100 = 200;        // circle blur = radius * boost/100
static int g_bandPx = 30;            // circle edge transition band px
static int g_focusPct = 60;          // background window reduction % (100 = off)
static bool g_running = true;
static bool g_overlayHidden = false;
static int g_ulwCount = 0;   // diag: ULW call count
static bool g_ovWdaTried = false, g_ovWdaOk = false;  // Route I: overlay self-exclude via WDA
static volatile LONG g_inCapture = 0;        // dual-hide capture in progress — suppress HIDE-event hide
static volatile DWORD g_captureDoneAt = 0;   // capture end tick — absorb late HIDE events (race fix)
static HWINEVENTHOOK g_hookFg = nullptr, g_hookObj = nullptr;
static const DWORD CAPTURE_EVENT_HOLD_MS = 150;   // absorb target HIDE events arriving shortly after capture

// ---- Cached-frame engine (v3.0) ----
static std::vector<BYTE> g_cache;      // captured BGRA frame, full-res
static bool g_cacheValid = false;
static RECT g_cachedRect = {};         // == frame(g_target) while cache valid
static DWORD g_renderDirty = 1;        // set on any param/mouse/focus change

// ---- Focus tracking (v3.0) ----
static bool g_focused = true;            // is g_target the foreground window
static DWORD g_focusChangedAt = 0;       // tick count when focus state flipped
static float g_curFocusF = 1.0f;         // current (lerped) focus factor
static float g_tintF = 1.0f;             // current (lerped) tint factor (1.0 focused, 0.5 unfocused)
static DWORD g_lastCaptureAt = 0;        // tick count of last dual-hide capture start
// target focus factor; 1.0 when focused, g_focusPct/100 when not
static float focus_target() { return g_focused ? 1.0f : g_focusPct / 100.0f; }
static float ease_out_cubic(float t) { return 1 - pow(1 - t, 3); }

// ---- Mouse-follow blur circle (v3.0) ----
static POINT g_circleCenter = {-1,-1};   // cursor pos relative to target rect; -1 = hidden
static DWORD g_lastMouseAt = 0;          // last cursor-MOVE tick (idle fade clock)
static float g_circleAlpha = 1.0f;       // 0..1, eased for idle fade
// radial smoothstep mask value at distance d (0 outside circle+band)
static float circle_mask(int d, int r, int band){
    float x = (float)(d - r) / band;
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    return 1 - x * x * (3 - 2 * x);
}

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

// Route I (2026-08-10): BitBlt capture — used when the target is
// WDA_EXCLUDEFROMCAPTURE. Verified: DDA shows BLACK in the excluded region
// (the DWM duplicate path doesn't reveal the background), but BitBlt (GDI
// screen DC) DOES show the true background (ImageGrab test: 186,146,175 vs
// DDA 15,5,25). WDA + BitBlt = live background without hiding the target.
static int g_captureFailSeq = 0;   // consecutive capture failures (diag + retry backoff)
bool capture_bitblt(int x, int y, int w, int h, std::vector<BYTE>& out) {
    HDC scr = GetDC(nullptr);
    if (!scr) return false;
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem); ReleaseDC(nullptr, scr);
        return false;
    }
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BOOL ok = BitBlt(mem, 0, 0, w, h, scr, x, y, SRCCOPY | CAPTUREBLT);
    if (ok && bits) {
        out.resize((size_t)w * h * 4);
        memcpy(out.data(), bits, out.size());
        // BitBlt gives BGRA; force alpha 255
        for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    }
    SelectObject(mem, old);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, scr);
    return ok;
}
bool capture_gpu(int x,int y,int w,int h,std::vector<BYTE>& out){
    g_dupl->ReleaseFrame();
    ComPtr<IDXGIResource> frameRes; DXGI_OUTDUPL_FRAME_INFO fi={};
    HRESULT hr=g_dupl->AcquireNextFrame(100,&fi,&frameRes);   // 16ms too short after a drag — DWM needs time to composite the hidden state
    if(FAILED(hr)){
        if(++g_captureFailSeq <= 10 || (g_captureFailSeq % 50) == 0){
            wprintf(L"[!] capture fail #%d hr=0x%08X\n", g_captureFailSeq, (unsigned)hr); fflush(stdout);
        }
        return false;
    }
    g_captureFailSeq = 0;
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
void blur(std::vector<BGRA>& buf,int w,int h,int radius){
    int r = radius / BLUR_PASSES; if (r < 1) r = 1;
    for(int i=0;i<BLUR_PASSES;i++){blur_h(buf,w,h,r);blur_v(buf,w,h,r);}
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
    BOOL ulwOk = UpdateLayeredWindow(g_overlay,scr,nullptr,&sz,mem,&pt,0,&blend,ULW_ALPHA);
    if (!ulwOk) { wprintf(L"[!] ULW failed err=%lu\n", GetLastError()); fflush(stdout); }
    SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);ReleaseDC(nullptr,scr);
    if (g_ulwCount++ < 20) { wprintf(L"[dbg] ULW %dx%d done\n", w, h); fflush(stdout); }
    // Route I (2026-08-10): try to exclude OUR OWN overlay from captures via
    // WDA. Earlier attempt at creation failed with err 8 (no composited
    // surface yet); now the overlay is actually rendering. If this succeeds,
    // live mode never needs to hide the overlay -> no flicker at all.
    if (!g_ovWdaTried) {
        g_ovWdaTried = true;
        if (SetWindowDisplayAffinity(g_overlay, 0x11)) {
            g_ovWdaOk = true;
            wprintf(L"[*] overlay WDA EXCLUDE OK — no hide needed in live mode\n");
            fflush(stdout);
        } else {
            wprintf(L"[!] overlay WDA err=%lu (live mode will hide via ULW)\n",
                    GetLastError()); fflush(stdout);
        }
    }
    // Non-layered windows (targets) accept it from their owner process — that
    // is the Route I injection path. Overlay self-exclusion is not possible
    // while it is layered; the feedback loop is mitigated by low wash alpha.
}

RECT frame(HWND hwnd){
    RECT r={};GetWindowRect(hwnd,&r);
    HMODULE dwm=LoadLibraryW(L"dwmapi.dll");
    if(dwm){auto fn=(HRESULT(WINAPI*)(HWND,DWORD,PVOID,DWORD))GetProcAddress(dwm,"DwmGetWindowAttribute");
        if(fn){RECT f={};if(SUCCEEDED(fn(hwnd,9,&f,sizeof(f)))&&f.right>f.left)r=f;}FreeLibrary(dwm);}
    return r;
}

// Place overlay directly ABOVE target: below the window that currently
// precedes the target in z-order (GW_HWNDPREV = the window ABOVE the target,
// per GetWindow's list-from-bottom semantics). Never moves the target.
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
        case EVENT_OBJECT_CLOAKED:
            // v3.0 race fix (2026-08-08): the dual-hide capture hides the target
            // itself; that HIDE event arrives asynchronously and used to call
            // hide_overlay() AFTER the capture's own show — leaving the overlay
            // permanently invisible (the "all effects unusable" symptom). Ignore
            // HIDE events that stem from our own capture (in progress or just done).
            if (!g_inCapture && GetTickCount() - g_captureDoneAt > CAPTURE_EVENT_HOLD_MS)
                hide_overlay();
            break;
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_UNCLOAKED:
            if(IsWindow(g_target)) show_overlay();
            break;
        }
    }
}

// Renders current params from g_cache into g_overlay (mask/tint/wash inside).
// Self-clearing gate: tick()'s `if(g_renderDirty)` sees 0 after we run.
// Cache holds the RAW captured frame — each render re-blurs from scratch,
// so repeated param changes never stack blur (zero recapture).
static void render_from_cache(){
    g_renderDirty = 0;
    if(!g_cacheValid || g_cache.empty()) return;
    RECT tr = g_cachedRect; int w = tr.right - tr.left, h = tr.bottom - tr.top;
    if(w<=0 || h<=0) return;
    if (g_cache.size() < (size_t)w*h*4) return;   // guard: capture byte-count mismatch
    // v3.0 pipeline: 1/2-scale downscale -> dual blur (base + circle boost)
    // -> radial-mask blend -> nearest upscale -> tint -> wash render
    // (2026-08-10: 1/4 -> 1/2 — 4x faster blur AND halves the visible pixel
    // blockiness the user saw at 4x4 grouping; still 4x cheaper than full-res.)
    int sw = w/2, sh = h/2; if(sw<8) sw=8; if(sh<8) sh=8;
    std::vector<BGRA> low(sw*sh);                 // box average of 2x2 groups
    for(int y=0;y<sh;y++)for(int x=0;x<sw;x++){
        int sx=x*2, sy=y*2; if(sx+2>w)sx=w-2; if(sy+2>h)sy=h-2;
        unsigned r=0,g=0,b=0;
        for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++){
            const BYTE* p=&g_cache[((sy+dy)*w+(sx+dx))*4];
            r+=p[2]; g+=p[1]; b+=p[0];
        }
        auto& o=low[y*sw+x]; o.r=(BYTE)(r/4); o.g=(BYTE)(g/4); o.b=(BYTE)(b/4); o.a=255;
    }
    std::vector<BGRA> base=low, boost=low;
    // (brief used windows.h `max()` macro — absent in this MinGW toolchain;
    //  equivalent explicit form, file idiom)
    int baseR = (int)(g_radius*g_curFocusF); if (baseR < 1) baseR = 1;   // Task 4 focus factor
    blur(base,sw,sh,baseR);
    int boostR = baseR*g_boostX100/100; if (boostR < baseR+1) boostR = baseR+1;
    if(g_circlePx>0 && g_circleAlpha>0.01f){ blur(boost,sw,sh,boostR); }
    else boost = base;
    std::vector<BGRA> out(w*h);
    // (brief Step 3 had `cc` test g_circleCenter.x by typo; both are -1
    // together so behavior-identical, fixed to y here)
    int cr = (g_circleCenter.x<0)?-1:g_circleCenter.x;
    int cc = (g_circleCenter.y<0)?-1:g_circleCenter.y;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        int ly = y/2; if (ly >= sh) ly = sh-1;
        int lx = x/2; if (lx >= sw) lx = sw-1;
        const BGRA& lo = base[ly*sw+lx];
        if(cr<0){ out[y*w+x]=lo;
            if(g_washAlpha==0) out[y*w+x].a=0;   // peephole, no circle yet: invisible until the circle activates
            continue; }
        float m = 0;
        if(g_circleAlpha>0.01f){
            int dx = x-cr, dy = y-cc;
            int d = (int)sqrtf((float)(dx*dx+dy*dy));
            m = circle_mask(d, g_circlePx, g_bandPx) * g_circleAlpha;
        }
        const BGRA& hi = boost[ly*sw+lx];
        out[y*w+x].r=(BYTE)(lo.r*(1-m)+hi.r*m);
        out[y*w+x].g=(BYTE)(lo.g*(1-m)+hi.g*m);
        out[y*w+x].b=(BYTE)(lo.b*(1-m)+hi.b*m);
        // v3.0 peephole (wash=0): per-pixel alpha = circle mask, constant alpha
        // 255 (render_layered already blends per-pixel x constant). Outside the
        // circle alpha=0 -> target shows 100% original; inside = local blur.
        out[y*w+x].a = (g_washAlpha==0) ? (BYTE)(255*m) : 255;
    }
    std::vector<BYTE> buf(w*h*4);
    memcpy(buf.data(), out.data(), w*h*4);      // memcpy wrapper (brief allows; avoids vector alias)
    BYTE constantAlpha = (BYTE)(g_washAlpha*g_curFocusF);
    if (g_washAlpha == 0) constantAlpha = 255;  // peephole: per-pixel mask drives alpha (blend already sets AC_SRC_ALPHA)
    render_layered(buf,w,h,constantAlpha);
    // v3.0 tint fix (2026-08-07): tint as a SECOND solid-black layer instead of
    // multiplying into the blur layer. Multiplying into blur was diluted by the
    // wash weight (effective tint = tintPct * wash/255 — at the user's wash=39,
    // tint 12% rendered ~1.8% and was invisible). A separate black layer at
    // tint% alpha darkens the whole window uniformly regardless of wash, so
    // tint=N is visibly N% black on any wash setting. Also honors the focus
    // factor g_tintF (background tint halves, per spec).
    int tintPct = (int)(g_tintPct*g_tintF);
    if (tintPct > 0) {
        std::vector<BYTE> tintLayer(w*h*4);     // solid black B,G,R,A = 0,0,0,255
        for (size_t i = 3; i < tintLayer.size(); i += 4) tintLayer[i] = 255;
        render_layered(tintLayer,w,h,(BYTE)(tintPct*255/100));
    }
}

void tick(){
    // 孤儿蒙版修复 (2026-08-12): 目标被销毁时 IsWindow 为假, 原代码直接 return,
    // 蒙版永远冻结在目标最后位置 (StickyNotes 启动时的临时 notelist 窗口即触发,
    // 左上角残留无归属蒙版)。目标销毁或不可见统一走 hide_overlay。
    // (EVENT_OBJECT_DESTROY 不在 0x8003..0x801C 钩子范围内, 事件分支是死代码,
    //  此处轮询兜底才是唯一可靠路径。)
    if(!IsWindow(g_overlay))return;
    if(!IsWindow(g_target)||!IsWindowVisible(g_target)){ hide_overlay(); return; }
    DWORD pid=0; GetWindowThreadProcessId(g_target,&pid);
    if(pid!=g_targetPid){ wprintf(L"[!] Target HWND recycled, exiting\n"); g_running=false; return; }
    show_overlay();
    RECT r=frame(g_target); int w=r.right-r.left,h=r.bottom-r.top;
    if(w<=0||h<=0)return;
    // Focus tracking: detect foreground flips, ease effect params toward target
    bool fg = GetForegroundWindow() == g_target;
    if (fg != g_focused) {
        // A dual-hide capture hides the target, so Windows reassigns the
        // foreground; the restored window rarely gets it back. Don't treat
        // an unfocus flip near a capture as real — the user may still be
        // looking at the window right after moving it.
        if (!fg && g_focused && GetTickCount() - g_lastCaptureAt < FOCUS_CAPTURE_HOLD_MS)
            fg = g_focused;          // hold focused state
        else { g_focused = fg; g_focusChangedAt = GetTickCount(); }
    }
    // ease toward target
    float t = 1.0f;
    if (g_focusChangedAt) {
        DWORD dt = GetTickCount() - g_focusChangedAt;
        t = dt >= 300 ? 1.0f : ease_out_cubic((float)dt / 300.0f);
    }
    float from = g_focusChangedAt ? g_curFocusF : focus_target();
    float to = focus_target();
    g_curFocusF = from + (to - from) * t;
    if (fabsf(g_curFocusF - to) > 0.001f) g_renderDirty = 1;
    // Tint factor: full profile value when focused, 0.5x when not (spec:
    // unfocused settled tint = g_tintPct * 0.5). Mirrors the F ease block —
    // same 300ms clock, first-run seeding from target, restart-from-current
    // on mid-ease flips so transitions stay continuous in both directions.
    float tint_to = g_focused ? 1.0f : 0.5f;
    float tint_from = g_focusChangedAt ? g_tintF : tint_to;
    g_tintF = tint_from + (tint_to - tint_from) * t;
    if (fabsf(g_tintF - tint_to) >= 0.001f) g_renderDirty = 1;
    // Mouse-follow blur circle (v3.0): track cursor over target rect.
    // Deviation from brief Step 2 snippet (documented in task-5-report): the
    // snippet stamped g_lastMouseAt every tick while inside and only marked
    // dirty when g_circleAlpha<1, which (a) made idle never exceed the poll
    // interval so the circle could never fade out, and (b) froze the circle
    // at its first position once alpha reached 1.0. Per the brief's behavior
    // paragraph (update center -> dirty; idle >1500ms -> fade), we mark dirty
    // on actual cursor-move only — movement re-anchors the circle, stillness
    // lets idle grow so the fade-out triggers.
    POINT cp; GetCursorPos(&cp);
    RECT tr = frame(g_target);
    bool inside = PtInRect(&tr, cp) != 0;
    if (inside && g_circlePx > 0) {
        POINT nc = {cp.x - tr.left, cp.y - tr.top};
        if (nc.x != g_circleCenter.x || nc.y != g_circleCenter.y) {
            g_circleCenter = nc;
            g_lastMouseAt = GetTickCount();
            g_renderDirty = 1;
        }
    } else if (g_circleCenter.x >= 0) {
        g_circleCenter = {-1, -1}; g_renderDirty = 1;
    }
    // idle fade: alpha eases to 0 after 1500ms without cursor movement, back
    // to 1 on movement; ~300ms total at the 33ms poll rate (rate per tick)
    if (g_lastMouseAt) {
        DWORD idle = GetTickCount() - g_lastMouseAt;
        float targetA = (g_circlePx > 0 && inside && idle < 1500) ? 1.0f : 0.0f;
        float rate = (float)POLL_INTERVAL_MS / 300.0f;
        if (g_circleAlpha < targetA) { g_circleAlpha = fminf(1.0f, g_circleAlpha + rate); g_renderDirty = 1; }
        else if (g_circleAlpha > targetA) { g_circleAlpha = fmaxf(0.0f, g_circleAlpha - rate); g_renderDirty = 1; }
    }
    bool moved = (r.left!=g_cachedRect.left||r.top!=g_cachedRect.top||
                  r.right!=g_cachedRect.right||r.bottom!=g_cachedRect.bottom) || !g_cacheValid;
    // Route I live mode (2026-08-10): when the target is WDA-excluded, re-capture
    // EVERY frame (no settle, no 5s timer). BitBlt of the region is cheap and
    // the hide/reveal cycle at 30Hz reads as smooth animation, not a 5s flash.
    DWORD liveAff = 0; GetWindowDisplayAffinity(g_target, &liveAff);
    bool liveMode = (liveAff == 0x11);
    if (liveMode) {
        g_cacheValid = false;              // force capture path every tick
        moved = true;
    } else {
        // v3.0 fix (2026-08-09): re-capture periodically even when stationary,
        // so dynamic wallpapers keep updating. 5s cadence keeps CPU cost low.
        static DWORD lastBgRefresh = 0;
        if (!moved && g_cacheValid && GetTickCount() - lastBgRefresh >= 5000) {
            lastBgRefresh = GetTickCount();
            moved = true;
            g_cacheValid = false;
        }
    }
    // v3.0 fixes (2026-08-08, user reports):
    // (a) "explorer freezes": reanchor()+SetWindowPos every 33ms tick flooded
    //     the target's window thread with z-order/position messages, starving
    //     its UI thread. Only reposition when actually out of place.
    // (b) "blur visible only ~50% of the time after a drag": z-order got
    //     perturbed by IME/DWM/other windows after the drag stopped, and since
    //     reanchor only ran on move it was never corrected. Cheap check every
    //     tick: GW_HWNDPREV(overlay) must be the TARGET (overlay directly above
    //     it); only SetWindowPos when violated.
    static RECT lastOvPos = {0,0,0,0};
    bool ovMoved = (r.left!=lastOvPos.left||r.top!=lastOvPos.top||
                    r.right!=lastOvPos.right||r.bottom!=lastOvPos.bottom);
    if (ovMoved || !g_cacheValid) {
        SetWindowPos(g_overlay,nullptr,r.left,r.top,w,h,SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOZORDER);
        lastOvPos = r;
        wprintf(L"[dbg] overlay repositioned to %ld,%ld\n", r.left, r.top); fflush(stdout);
    }
    if (g_overlay && IsWindow(g_overlay) &&
        GetWindow(g_overlay, GW_HWNDNEXT) != g_target) {
        reanchor();   // z-order drifted — restore overlay directly above target
    }
    // v3.0 focus-flash fix: during the 300ms focus ease the window's extended
    // frame bounds can jitter by a couple px (activation/deactivation changes
    // the shadow/border metrics), which would otherwise trip the dual-hide
    // capture below — the hide+show makes the window "flash like lightning"
    // on every focus switch (user report 2026-08-07). Defer capture while the
    // focus transition is settling; position already follows via SetWindowPos.
    bool focusEasing = g_focusChangedAt && (GetTickCount() - g_focusChangedAt) < 300;
    if(!moved || focusEasing){
        // also throttle re-render churn during the ease: 300ms at 33ms poll =
        // ~9 UpdateLayeredWindow submissions; 3-4 suffices for a smooth fade
        // and halves the visible flicker on fast switches.
        if(g_renderDirty && !focusEasing) render_from_cache();
        else if(g_renderDirty && focusEasing){
            static DWORD lastEaseRender=0; DWORD now=GetTickCount();
            if(now-lastEaseRender>=80){ render_from_cache(); lastEaseRender=now; }
        }
        return;
    }
    // Move or first frame: reposition overlay only, defer capture until stable
    static int settleCount=0; static RECT lastRect={};
    // v3.0 fix: capture-failure backoff. If capture keeps failing (e.g. DDA
    // contention with several overlays, or a transient GPU state), retrying
    // every ~100ms re-runs the dual-hide (target flashes off/on repeatedly —
    // the user's "windows repeatedly close and reopen" symptom). Back off
    // between attempts while failing; only the success path resets it.
    // 2026-08-08: 2s -> 800ms — the old window made "drag then release"
    // updates feel ~1s late (user report); 800ms still prevents a storm while
    // the stale cache keeps rendering (no visual flash).
    static DWORD lastFailAt=0;
    if (g_captureFailSeq > 0 && GetTickCount() - lastFailAt < 800) return;
    if (!liveMode) {
        bool moved2 = (r.left!=lastRect.left||r.top!=lastRect.top||
                       r.right!=lastRect.right||r.bottom!=lastRect.bottom);
        if (moved2) { static int dbgMove=0; if ((++dbgMove % 15) == 1) { wprintf(L"[dbg] moving %ld,%ld -> %ld,%ld\n", lastRect.left, lastRect.top, r.left, r.top); fflush(stdout); } }
        lastRect=r;
        if(moved2){ settleCount=0; return; }
        settleCount++;
        if(settleCount<2) return;                    // wait 2 stable frames (~66ms); 3 frames felt ~1s late after drags (user report)
    }
    g_cacheValid=false;
    g_lastCaptureAt = GetTickCount();            // capture start: suppress false unfocus
    InterlockedExchange(&g_inCapture, 1);
    // Route I (2026-08-10): if the target is WDA_EXCLUDEFROMCAPTURE (set by
    // the injected affinity DLL), it is already invisible to DDA/BitBlt — NO
    // hiding needed for the target. But the OVERLAY (our own layered window,
    // which cannot take WDA — err 8) sits above the target and would be
    // captured into the frame (feedback loop). Hide it via LWA_ALPHA=0 for
    // the capture instant (our own window, dynamic content — the one-frame
    // dip is far less visible than SW_HIDE).
    DWORD taff = 0; GetWindowDisplayAffinity(g_target, &taff);
    bool wdaExcluded = (taff == 0x11);
    bool overlayHidViaAlpha = false;
    if (wdaExcluded && g_ovWdaOk) {
        // Live mode with overlay self-excluded: NOTHING to hide — the overlay
        // is already invisible to BitBlt via its own WDA. Zero flicker.
    } else if (wdaExcluded) {
        // Hide the overlay for the capture by re-rendering it at alpha 0 via
        // ULW itself — NOT SetLayeredWindowAttributes. Mixing LWA_ALPHA with
        // UpdateLayeredWindow on the same window makes ULW fail with err 87
        // (verified). ULW with SourceConstantAlpha=0 is the clean hide.
        std::vector<BYTE> zero(w * h * 4, 0);
        render_layered(zero, w, h, 0);
        overlayHidViaAlpha = true;
    } else {
        ShowWindow(g_overlay, SW_HIDE);
    }
    LONG savedEx = 0; bool wasLayered = false;
    BYTE savedAlpha = 255; DWORD savedFlags = 0;
    if (!wdaExcluded) {
        // v3.0 fix (2026-08-09, taskbar-reorder root): do NOT ShowWindow(SW_HIDE)
        // the TARGET. SW_HIDE fires HSHELL_WINDOWDESTROYED, Explorer removes the
        // taskbar button and re-adds it at the end on restore ("window closed and
        // reopened, taskbar order reset" — user report). Instead make the target
        // INVISIBLE via LWA_ALPHA=0: it drops out of the composition (capture sees
        // the true background) while IsWindowVisible stays TRUE and the Shell is
        // untouched. Restore the exact prior layered state afterwards.
        savedEx = GetWindowLongW(g_target, GWL_EXSTYLE);
        wasLayered = (savedEx & WS_EX_LAYERED) != 0;
        if (wasLayered)
            GetLayeredWindowAttributes(g_target, nullptr, &savedAlpha, &savedFlags);
        if (!wasLayered) SetWindowLongW(g_target, GWL_EXSTYLE, savedEx | WS_EX_LAYERED);
        SetLayeredWindowAttributes(g_target, 0, 0, LWA_ALPHA);
    }
    // v3.0 fix (2026-08-08, user report "blur only blurs the window's own
    // content"): DDA produces a new frame only AFTER DWM re-composites the
    // desktop. Give DWM real time to composite: several DwmFlush + waits.
    // (Live mode skips this — nothing was hidden, DWM needs no recomposite,
    // except the overlay's own ULW(0) hide must reach DWM: one flush suffices.)
    if (!liveMode) {
        for (int i = 0; i < 5; i++) {
            DwmFlush();
            Sleep(8);
        }
    } else {
        DwmFlush();
    }
    g_cache.clear();
    // Route I: WDA-excluded target -> BitBlt (DDA shows black there; BitBlt
    // reveals the true background). Non-WDA -> DDA as before.
    bool ok = wdaExcluded
        ? capture_bitblt(r.left, r.top, w, h, g_cache)
        : capture_gpu(r.left-g_outX, r.top-g_outY, w, h, g_cache);
    wprintf(L"[dbg] capture %s at (%ld,%ld) %dx%d wda=%d\n", ok ? "OK" : "FAIL",
            r.left, r.top, w, h, wdaExcluded ? 1 : 0); fflush(stdout);
    if (ok && g_cache.size() >= 16) {
        size_t idx = ((size_t)(h/2) * w + w/2) * 4;
        wprintf(L"[dbg]   frame center=(%u,%u,%u)\n", g_cache[idx], g_cache[idx+1], g_cache[idx+2]);
        fflush(stdout);
    }
    if (!wdaExcluded) {
        // Restore target layered state (mirror of the LWA_ALPHA=0 hide above).
        if (wasLayered)
            SetLayeredWindowAttributes(g_target, 0, savedAlpha, savedFlags);
        else {
            SetLayeredWindowAttributes(g_target, 0, 255, LWA_ALPHA);
            SetWindowLongW(g_target, GWL_EXSTYLE, savedEx);
        }
    }
    if (!overlayHidViaAlpha)
        ShowWindow(g_overlay,SW_SHOWNOACTIVATE);
    // (WDA mode: overlay was hidden via ULW alpha=0; the render_from_cache
    // right below re-renders it at full wash alpha — no separate restore.)
    reanchor();
    InterlockedExchange(&g_inCapture, 0);
    g_captureDoneAt = GetTickCount();            // absorb late HIDE events (race fix)
    if (!ok) lastFailAt = GetTickCount();        // backoff before next retry
    // Capture's own SW_HIDE/SW_SHOW can race the event callback; end-of-capture
    // always leaves the overlay visible (target is visible at this point).
    if (g_overlayHidden) show_overlay();
    if(ok&&!g_cache.empty()){ g_cacheValid=true; g_cachedRect=r; g_renderDirty=1; render_from_cache();
        wprintf(L"[dbg] rendered after capture\n"); fflush(stdout); }
    settleCount=0;
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

// 父进程看护 (2026-08-12): 同 acrylic_overlay, 父进程被强杀时叠加层自行退出,
// 杜绝孤儿蒙版堆积。
static void watch_parent_exit() {
    DWORD parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {sizeof(pe)};
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == GetCurrentProcessId()) {
                    parent = pe.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (!parent) return;
    HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, parent);
    if (!hp) return;
    WaitForSingleObject(hp, INFINITE);
    CloseHandle(hp);
    g_running = false;
}

int main(int argc,char*argv[]){
    if(argc<3){fprintf(stderr,"Usage: crisp_overlay.exe <hwnd_hex> <wash 0-255> [tint 0-100] [radius 1-120] [circle_px] [boost_x100] [band_px] [focus_pct]\n");return 1;}
    g_target=(HWND)(ULONG_PTR)strtoull(argv[1],nullptr,16);
    if(!IsWindow(g_target)){fprintf(stderr,"[x] Invalid HWND\n");return 1;}
    GetWindowThreadProcessId(g_target,&g_targetPid);
    g_washAlpha=(BYTE)atoi(argv[2]);
    if(argc>=4){g_tintPct=atoi(argv[3]);if(g_tintPct<0)g_tintPct=0;if(g_tintPct>100)g_tintPct=100;}
    if (argc >= 5) { g_radius = atoi(argv[4]); if (g_radius < 1) g_radius = 1; if (g_radius > 120) g_radius = 120; }
    if (argc >= 6) { g_circlePx = atoi(argv[5]); if (g_circlePx < 0) g_circlePx = 0; }
    if (argc >= 7) { g_boostX100 = atoi(argv[6]); if (g_boostX100 < 100) g_boostX100 = 100; }
    if (argc >= 8) { g_bandPx = atoi(argv[7]); if (g_bandPx < 5) g_bandPx = 5; }
    if (argc >= 9) { g_focusPct = atoi(argv[8]); if (g_focusPct < 10) g_focusPct = 10; if (g_focusPct > 100) g_focusPct = 100; }

    init_gpu();

    const wchar_t* CN=L"CrispOverlayClass";
    WNDCLASSW wc={};wc.lpfnWndProc=OverlayWndProc;
    wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName=CN;
    RegisterClassW(&wc);
    g_overlay=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,
        CN,L"CrispOverlay",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    if(!g_overlay){fprintf(stderr,"[x] Failed to create overlay (err=%lu)\n",GetLastError());return 1;}

    wchar_t title[256];GetWindowTextW(g_target,title,256);
    wprintf(L"[*] Target: %s\n",title);fflush(stdout);
    wprintf(L"[*] Pipeline: DDA capture -> CPU box blur %dpx x%d -> tint %d%% -> wash alpha %u\n",
            g_radius,BLUR_PASSES,g_tintPct,g_washAlpha);fflush(stdout);

    g_hookFg=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,
        nullptr,WinEventProc,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    g_hookObj=SetWinEventHook(EVENT_OBJECT_HIDE,EVENT_OBJECT_UNCLOAKED,
        nullptr,WinEventProc,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);

    // A previous overlay may have died mid-capture after dual-hiding the
    // target (ShowWindow(g_target,SW_HIDE) in tick()), leaving it stuck
    // hidden. win2blur never intentionally leaves a target hidden — restore
    // it on startup so the window is never "lost" (ghost process symptom).
    if (IsWindow(g_target) && !IsWindowVisible(g_target)) ShowWindow(g_target, SW_SHOW);

    ShowWindow(g_overlay,SW_SHOWNOACTIVATE);
    tick();  // first capture immediately
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        watch_parent_exit(); return 0;
    }, nullptr, 0, nullptr);
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
