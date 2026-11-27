// Applying DISPLAY OPTIONS while the game runs.
//
// At startup the game reads settings.xml into 0xEEC9C8 and copies it into the engine config
// object [0xBC3E28] (0x417410):
//   +0x40 width   +0x44 height   +0xD4 / +0xD8 display mode   +0xDC vsync   +0xE0 bpp
//   +0x50 ambient occlusion   +0x54 anti-aliasing   +0x58 low models (model level 0)
//   +0x5C / +0x60 motion blur   +0x64 screen splash   +0x68 shadow level (0/1/2)
// Screen splash, motion blur and anti-aliasing are also checked directly from the config while
// racing. The scene renderer (class with vtable 0x9CC490, 0x2C0 bytes) copies ambient
// occlusion, motion blur, anti-aliasing and the shadow level when it builds its render
// pipeline (0x55ACD0); its vtable slot 0x28 (0x5401D0) queues that build on the render thread.
//
// So on Accept: write the new values into the config and into the renderers' own copies
// (asking a renderer to rebuild crashed). Renderers are tracked by wrapping the three calls to
// their constructor. Shadows are set up by the pipeline build, so they follow on the next one.
// Resolution, display mode and V-Sync go through the engine's own device reset (see below);
// texture / water / model quality are read when assets load.
#include "game.h"

#define ENGINE_CONFIG       (*(BYTE**)0x00BC3E28)
#define RENDERER_VTABLE     0x009CC490
#define ADDR_RENDERER_CTOR  0x00540F10
#define MAX_RENDERERS       16

static const DWORD s_ctorCalls[] = { 0x004077DE, 0x00407843, 0x00541A29 };
static void* s_renderers[MAX_RENDERERS];

typedef void* (__thiscall* RendererCtor_t)(void* self);

static void* __fastcall RendererCtor(void* self, void* edx)
{
    (void)edx;
    void* result = ((RendererCtor_t)ADDR_RENDERER_CTOR)(self);
    for (int i = 0; i < MAX_RENDERERS; i++)
    {
        if (!s_renderers[i] || *(DWORD*)s_renderers[i] != RENDERER_VTABLE)
        {
            s_renderers[i] = self;
            break;
        }
    }
    return result;
}

static void __cdecl Hook_RequestResize(void* display, const int* size);

int LiveInstall(void)
{
    for (size_t i = 0; i < sizeof(s_ctorCalls) / sizeof(s_ctorCalls[0]); i++)
    {
        unsigned char call[5] = { 0xE8 };
        *(LONG*)(call + 1) = (LONG)(ADDR_RENDERER_CTOR - (s_ctorCalls[i] + 5));
        if (!PatchExpect(s_ctorCalls[i], call, 5))
        {
            Log(L"Live settings: unexpected code at 0x%08X; renderer rebuild disabled", s_ctorCalls[i]);
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof(s_ctorCalls) / sizeof(s_ctorCalls[0]); i++)
        PatchRel32(s_ctorCalls[i], 0xE8, (void*)RendererCtor, 5);
    // mov eax, [esp+8] / mov ecx, [eax]
    static const unsigned char resizeEntry[6] = { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x08 };
    if (PatchExpect(0x00443440, resizeEntry, 6))
        PatchRel32(0x00443440, 0xE9, (void*)Hook_RequestResize, 6);
    else
        Log(L"Live settings: unexpected code at 0x443440; borderless resolution follows the window");
    Log(L"Live settings: installed");
    return 1;
}

// A field of a renderer-owned object, written only when it holds the value the config held
// before (so a wrong guess about the object can't scribble over something else).
static int SetOwned(void* renderer, int holder, int field, int oldValue, int value)
{
    BYTE* obj = *(BYTE**)((BYTE*)renderer + holder);
    if (!obj || IsBadWritePtr(obj + field, 4))
        return 0;
    int* p = (int*)(obj + field);
    if (*p != oldValue && *p != value)
        return 0;
    *p = value;
    return 1;
}

// ---- resolution / display mode / V-Sync ----
// The engine keeps its D3DPRESENT_PARAMETERS at 0xE9901C and resets the device with them
// ("SlDisplay_Platform::ResetDevice", 0x4434B0) at the next Present (0x443C20) whenever
// 0xE9A904 is set. Its window-size handler (0x443440) asks for a reset with the new client
// size the same way (0xE99014 / 0xE99018, 0xE9A8FC).
#define PP_WIDTH        (*(UINT*)0x00E9901C)
#define PP_HEIGHT       (*(UINT*)0x00E99020)
#define PP_WINDOW       (*(HWND*)0x00E99038)
#define PP_WINDOWED     (*(BOOL*)0x00E9903C)
#define PP_REFRESH      (*(UINT*)0x00E9904C)
#define PP_INTERVAL     (*(UINT*)0x00E99050)
#define RESET_PENDING   (*(int*)0x00E9A904)
#define D3D_DEVICE      (*(void**)0x00E99054)

static HWND GameWindow(void)
{
    // IDirect3DDevice9::GetCreationParameters (slot 9): the focus window.
    struct { UINT adapter; int type; HWND focus; DWORD flags; } cp;
    void* dev = D3D_DEVICE;
    if (!dev)
        return NULL;
    typedef long (__stdcall* GetCP_t)(void*, void*);
    if (((GetCP_t)((*(void***)dev)[9]))(dev, &cp) < 0)
        return NULL;
    return cp.focus;
}

// Window changes have to be made on the window's own thread: done from the menu's thread,
// SetWindowLong / SetWindowPos wait for the window thread, which was waiting for the menu's
// thread - the game froze. So the window is subclassed and the change is posted to it.
#define WM_MO_DISPLAY (WM_APP + 0x51)
static WNDPROC s_origProc;
static HWND s_subclassed;
static VideoSettings s_pending;

// Size requests from the window (0x443440, cdecl (display, SIZE*)): once a resolution was chosen
// here, the reset uses it instead of the window's client size (borderless renders at the chosen
// resolution and is stretched to the desktop-sized window).
static int s_forceW, s_forceH;

static void __cdecl Hook_RequestResize(void* display, const int* size)
{
    (void)display;
    *(int*)0x00E99014 = s_forceW ? s_forceW : size[0];
    *(int*)0x00E99018 = s_forceH ? s_forceH : size[1];
    *(int*)0x00E9A8FC = 1;
}

static void DoDisplayChange(HWND wnd, const VideoSettings* s)
{
    int screenW = GetSystemMetrics(SM_CXSCREEN), screenH = GetSystemMetrics(SM_CYSCREEN);
    // Borderless: the window covers the desktop, the picture is rendered at the chosen
    // resolution and stretched to it when presented.
    int w = s->width, h = s->height;
    BOOL windowed = s->displayMode != 1;
    UINT interval = s->vsync ? 1u : 0x80000000u;   // D3DPRESENT_INTERVAL_ONE / _IMMEDIATE
    LONG oldStyle = GetWindowLongW(wnd, GWL_STYLE);
    s_forceW = w;
    s_forceH = h;

    PP_WIDTH = (UINT)w;
    PP_HEIGHT = (UINT)h;
    PP_WINDOWED = windowed;
    PP_REFRESH = windowed ? 0 : (UINT)(s->refresh > 0 ? s->refresh : 60);
    PP_WINDOW = windowed ? NULL : wnd;
    PP_INTERVAL = interval;

    BYTE* cfg = ENGINE_CONFIG;
    if (cfg)
    {
        *(int*)(cfg + 0x40) = w;
        *(int*)(cfg + 0x44) = h;
        *(int*)(cfg + 0xD4) = s->displayMode != 0;
        *(int*)(cfg + 0xD8) = s->displayMode == 2;
        *(int*)(cfg + 0xDC) = s->vsync;
    }

    // A normal captioned window centred on the desktop, or a popup covering the screen.
    LONG style = oldStyle;
    if (IsZoomed(wnd))
        ShowWindow(wnd, SW_RESTORE);
    SetWindowLongW(wnd, GWL_EXSTYLE, GetWindowLongW(wnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
    if (s->displayMode == 0)
    {
        style = (style & ~(WS_POPUP | WS_MAXIMIZE)) | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        SetWindowLongW(wnd, GWL_STYLE, style);
        RECT r = { 0, 0, w, h };
        AdjustWindowRect(&r, (DWORD)style, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        SetWindowPos(wnd, HWND_NOTOPMOST, (screenW - ww) / 2, (screenH - wh) / 2, ww, wh,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else
    {
        style = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MAXIMIZE)) | WS_POPUP;
        SetWindowLongW(wnd, GWL_STYLE, style);
        int ww = s->displayMode == 2 ? screenW : w, wh = s->displayMode == 2 ? screenH : h;
        SetWindowPos(wnd, HWND_TOP, 0, 0, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    // The window's size handler asks for its own reset with the client size (0xE9A8FC), which
    // would replace the chosen resolution (borderless) - cancel it; the values above are used.
    PP_WIDTH = (UINT)w;
    PP_HEIGHT = (UINT)h;
    *(int*)0x00E9A8FC = 0;
    RESET_PENDING = 1;
    RECT wr;
    GetWindowRect(wnd, &wr);
    Log(L"Live settings: display %dx%d %s, vsync %d - device reset requested (style %08lX -> %08lX, window %ldx%ld at %ld,%ld)",
        w, h, s->displayMode == 0 ? L"windowed" : (s->displayMode == 1 ? L"fullscreen" : L"borderless"), s->vsync,
        (unsigned long)oldStyle, (unsigned long)GetWindowLongW(wnd, GWL_STYLE),
        wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top);
}

static LRESULT CALLBACK DisplayProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MO_DISPLAY)
    {
        VideoSettings s = s_pending;
        DoDisplayChange(wnd, &s);
        return 0;
    }
    return CallWindowProcW(s_origProc, wnd, msg, wp, lp);
}

static void ApplyDisplay(const VideoSettings* s)
{
    HWND wnd = GameWindow();
    if (!wnd || !D3D_DEVICE)
    {
        Log(L"Live settings: no device / window; display change left for the next start");
        return;
    }
    static int lastMode = -1;
    UINT interval = s->vsync ? 1u : 0x80000000u;
    if (PP_WIDTH == (UINT)s->width && PP_HEIGHT == (UINT)s->height && PP_WINDOWED == (s->displayMode != 1) &&
        PP_INTERVAL == interval && lastMode == s->displayMode)
        return;   // nothing to do
    lastMode = s->displayMode;

    if (s_subclassed != wnd)
    {
        // Replacing the window procedure sends no messages, so it is safe from this thread.
        s_origProc = (WNDPROC)SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)DisplayProc);
        s_subclassed = s_origProc ? wnd : NULL;
        if (!s_subclassed)
        {
            Log(L"Live settings: could not hook the game window; display change left for the next start");
            return;
        }
    }
    s_pending = *s;
    PostMessageW(wnd, WM_MO_DISPLAY, 0, 0);
}

void LiveApply(const VideoSettings* s)
{
    BYTE* cfg = ENGINE_CONFIG;
    if (!cfg)
        return;
    ApplyDisplay(s);
    int oldAO = *(int*)(cfg + 0x50), oldAA = *(int*)(cfg + 0x54);
    int oldMB1 = *(int*)(cfg + 0x5C), oldMB2 = *(int*)(cfg + 0x60);

    // Read by the game every frame: screen splash, motion blur, anti-aliasing checks.
    *(int*)(cfg + 0x50) = s->ambientOcclusion;
    *(int*)(cfg + 0x54) = s->antialias;
    *(int*)(cfg + 0x58) = s->model == 0;
    *(int*)(cfg + 0x5C) = s->motionBlur;
    *(int*)(cfg + 0x60) = s->motionBlur;
    *(int*)(cfg + 0x64) = s->screenSplash;
    *(int*)(cfg + 0x68) = s->shadow;

    // The scene renderer keeps its own copies (made by its pipeline build, 0x55AFD0):
    //   *(renderer + 0x27C) + 0x158 ambient occlusion, +0x15C / +0x160 motion blur
    //   *(renderer + 0x260) + 0x260 anti-aliasing
    // Rebuilding the pipeline while it is in use crashed, so these copies are updated instead.
    int set = 0, renderers = 0;
    for (int i = 0; i < MAX_RENDERERS; i++)
    {
        void* r = s_renderers[i];
        if (!r || *(DWORD*)r != RENDERER_VTABLE)
        {
            s_renderers[i] = NULL;   // destroyed
            continue;
        }
        renderers++;
        set += SetOwned(r, 0x27C, 0x158, oldAO, s->ambientOcclusion);
        set += SetOwned(r, 0x27C, 0x15C, oldMB2, s->motionBlur);
        set += SetOwned(r, 0x27C, 0x160, oldMB1, s->motionBlur);
        set += SetOwned(r, 0x260, 0x260, oldAA, s->antialias);
    }
    Log(L"Live settings: applied (AO %d, AA %d, motion blur %d, splash %d, shadow %d, low models %d); %d renderer(s), %d copy field(s) set",
        s->ambientOcclusion, s->antialias, s->motionBlur, s->screenSplash, s->shadow, s->model == 0, renderers, set);
}
