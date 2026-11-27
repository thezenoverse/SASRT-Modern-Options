// Hooks IDirect3DDevice9::Present / Reset by patching the device vtable inside d3d9.dll.
// The vtable is found by creating a throwaway device; all devices of the same class share it,
// so the game's device is hooked even if it was created before us.
#define COBJMACROS
#define CINTERFACE
#include "common.h"
#include <d3d9.h>

#define VT_RESET 16
#define VT_PRESENT 17

typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* SwapPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
#define VT_SWAPCHAIN_PRESENT 3
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT);

static Present_t s_realPresent = NULL;
static Reset_t s_realReset = NULL;
static SwapPresent_t s_realSwapPresent = NULL;
static volatile LONG s_lastPresentTick = 0;
static volatile LONG s_captureRequested = 0;
static wchar_t s_capturePath[MAX_PATH];
static CRITICAL_SECTION s_lock;

static const char* FormatName(D3DFORMAT f)
{
    switch (f)
    {
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_X1R5G5B5: return "X1R5G5B5";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    default: return "other";
    }
}

// Converts one locked surface row into BGRA8.
static int ConvertRow(D3DFORMAT fmt, const unsigned char* src, unsigned char* dst, int width)
{
    switch (fmt)
    {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
        memcpy(dst, src, (size_t)width * 4);
        return 1;
    case D3DFMT_A2R10G10B10:
        for (int x = 0; x < width; x++)
        {
            unsigned int p = ((const unsigned int*)src)[x];
            dst[x * 4 + 2] = (unsigned char)(((p >> 20) & 0x3FF) >> 2);
            dst[x * 4 + 1] = (unsigned char)(((p >> 10) & 0x3FF) >> 2);
            dst[x * 4 + 0] = (unsigned char)((p & 0x3FF) >> 2);
            dst[x * 4 + 3] = 255;
        }
        return 1;
    case D3DFMT_R5G6B5:
        for (int x = 0; x < width; x++)
        {
            unsigned short p = ((const unsigned short*)src)[x];
            dst[x * 4 + 2] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
            dst[x * 4 + 1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
            dst[x * 4 + 0] = (unsigned char)((p & 0x1F) * 255 / 31);
            dst[x * 4 + 3] = 255;
        }
        return 1;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        for (int x = 0; x < width; x++)
        {
            unsigned short p = ((const unsigned short*)src)[x];
            dst[x * 4 + 2] = (unsigned char)(((p >> 10) & 0x1F) * 255 / 31);
            dst[x * 4 + 1] = (unsigned char)(((p >> 5) & 0x1F) * 255 / 31);
            dst[x * 4 + 0] = (unsigned char)((p & 0x1F) * 255 / 31);
            dst[x * 4 + 3] = 255;
        }
        return 1;
    default:
        return 0;
    }
}

static void CaptureBackBuffer(IDirect3DDevice9* dev, const wchar_t* path)
{
    IDirect3DSurface9* backBuffer = NULL;
    IDirect3DSurface9* resolved = NULL;
    IDirect3DSurface9* sysmem = NULL;
    D3DSURFACE_DESC desc;

    HRESULT hr = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr))
    {
        Log(L"Capture: GetBackBuffer failed (0x%08lX)", hr);
        return;
    }
    IDirect3DSurface9_GetDesc(backBuffer, &desc);

    IDirect3DSurface9* source = backBuffer;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
    {
        // Multisampled surfaces can't be read back directly; resolve first.
        hr = IDirect3DDevice9_CreateRenderTarget(dev, desc.Width, desc.Height, desc.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, NULL);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_StretchRect(dev, backBuffer, NULL, resolved, NULL, D3DTEXF_NONE);
        if (FAILED(hr))
        {
            Log(L"Capture: resolving MSAA back buffer failed (0x%08lX)", hr);
            goto done;
        }
        source = resolved;
    }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, desc.Width, desc.Height, desc.Format,
        D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTargetData(dev, source, sysmem);
    if (FAILED(hr))
    {
        Log(L"Capture: reading back buffer failed (0x%08lX)", hr);
        goto done;
    }

    D3DLOCKED_RECT lr;
    hr = IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr))
    {
        Log(L"Capture: LockRect failed (0x%08lX)", hr);
        goto done;
    }

    int w = (int)desc.Width, h = (int)desc.Height;
    unsigned char* pixels = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
    int converted = pixels != NULL;
    for (int y = 0; converted && y < h; y++)
        converted = ConvertRow(desc.Format, (const unsigned char*)lr.pBits + (size_t)y * (size_t)lr.Pitch,
            pixels + (size_t)y * (size_t)w * 4, w);
    IDirect3DSurface9_UnlockRect(sysmem);

    if (!converted)
        Log(L"Capture: unsupported back buffer format %d (%hs)", (int)desc.Format, FormatName(desc.Format));
    else if (WritePngBgra(path, pixels, w, h, w * 4))
        Log(L"Saved screenshot %s (%dx%d, %hs)", path, w, h, FormatName(desc.Format));
    else
        Log(L"Capture: failed to write %s", path);
    free(pixels);

done:
    if (sysmem) IDirect3DSurface9_Release(sysmem);
    if (resolved) IDirect3DSurface9_Release(resolved);
    if (backBuffer) IDirect3DSurface9_Release(backBuffer);
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    InterlockedExchange(&s_lastPresentTick, (LONG)GetTickCount());

    if (InterlockedCompareExchange(&s_captureRequested, 0, 1) == 1)
    {
        wchar_t path[MAX_PATH];
        EnterCriticalSection(&s_lock);
        wcscpy(path, s_capturePath);
        LeaveCriticalSection(&s_lock);
        CaptureBackBuffer(dev, path);
    }

    return s_realPresent(dev, src, dst, wnd, dirty);
}

// Some games present through the swap chain instead of the device.
static HRESULT STDMETHODCALLTYPE Hook_SwapPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
    const RGNDATA* dirty, DWORD flags)
{
    InterlockedExchange(&s_lastPresentTick, (LONG)GetTickCount());

    if (InterlockedCompareExchange(&s_captureRequested, 0, 1) == 1)
    {
        wchar_t path[MAX_PATH];
        EnterCriticalSection(&s_lock);
        wcscpy(path, s_capturePath);
        LeaveCriticalSection(&s_lock);

        IDirect3DDevice9* dev = NULL;
        if (SUCCEEDED(IDirect3DSwapChain9_GetDevice(sc, &dev)) && dev)
        {
            CaptureBackBuffer(dev, path);
            IDirect3DDevice9_Release(dev);
        }
    }

    return s_realSwapPresent(sc, src, dst, wnd, dirty, flags);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    HRESULT hr = s_realReset(dev, pp);
    if (pp)
        Log(L"Device reset: %ux%u, windowed=%d -> 0x%08lX", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed, hr);
    return hr;
}

static int PatchVtable(void** vtable, int index, void* hook, void** original)
{
    DWORD old;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return 0;
    if (vtable[index] != hook)
    {
        *original = vtable[index];
        vtable[index] = hook;
    }
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    return 1;
}

int D3D9HookInstall(void)
{
    InitializeCriticalSection(&s_lock);

    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    Direct3DCreate9_t create = d3d9 ? (Direct3DCreate9_t)(void*)GetProcAddress(d3d9, "Direct3DCreate9") : NULL;
    if (!create)
    {
        Log(L"D3D9: d3d9.dll / Direct3DCreate9 not available");
        return 0;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ModernOptionsDummy";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    IDirect3DDevice9* dev = NULL;
    if (d3d && wnd)
    {
        D3DPRESENT_PARAMETERS pp;
        ZeroMemory(&pp, sizeof(pp));
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.hDeviceWindow = wnd;

        // HAL matches the game's device class; NULLREF is a fallback when HAL creation is refused.
        static const D3DDEVTYPE types[] = { D3DDEVTYPE_HAL, D3DDEVTYPE_NULLREF };
        for (int i = 0; i < 2 && !dev; i++)
        {
            HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, types[i], wnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
            Log(L"D3D9: dummy %s device -> 0x%08lX", i == 0 ? L"HAL" : L"NULLREF", hr);
        }
    }

    int ok = 0;
    if (dev)
    {
        void** vtable = *(void***)dev;
        ok = PatchVtable(vtable, VT_PRESENT, (void*)Hook_Present, (void**)&s_realPresent) &&
            PatchVtable(vtable, VT_RESET, (void*)Hook_Reset, (void**)&s_realReset);

        IDirect3DSwapChain9* sc = NULL;
        if (ok && SUCCEEDED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) && sc)
        {
            void** scVtable = *(void***)sc;
            if (PatchVtable(scVtable, VT_SWAPCHAIN_PRESENT, (void*)Hook_SwapPresent, (void**)&s_realSwapPresent))
                Log(L"D3D9: swap chain Present hooked");
            IDirect3DSwapChain9_Release(sc);
        }
        IDirect3DDevice9_Release(dev);
    }
    if (d3d)
        IDirect3D9_Release(d3d);
    if (wnd)
        DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Log(ok ? L"D3D9: Present/Reset hooked" : L"D3D9: hook failed; F5 will use the GDI fallback");
    return ok;
}

int D3D9HookActive(void)
{
    LONG last = s_lastPresentTick;
    return last != 0 && (LONG)(GetTickCount() - (DWORD)last) < 1000;
}

void D3D9RequestCapture(const wchar_t* path)
{
    EnterCriticalSection(&s_lock);
    wcsncpy(s_capturePath, path, MAX_PATH - 1);
    s_capturePath[MAX_PATH - 1] = 0;
    LeaveCriticalSection(&s_lock);
    InterlockedExchange(&s_captureRequested, 1);
}
