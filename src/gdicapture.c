// Fallback screenshot through GDI, used when the D3D9 Present hook isn't seeing frames.
// Works for windowed / borderless; exclusive fullscreen usually comes out black.
#include "common.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

typedef struct FindData
{
    DWORD pid;
    HWND best;
    LONG bestArea;
} FindData;

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
{
    FindData* fd = (FindData*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != fd->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;

    RECT rc;
    if (GetClientRect(hwnd, &rc))
    {
        LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > fd->bestArea)
        {
            fd->best = hwnd;
            fd->bestArea = area;
        }
    }
    return TRUE;
}

HWND FindGameWindow(void)
{
    FindData fd = { GetCurrentProcessId(), NULL, 0 };
    EnumWindows(EnumProc, (LPARAM)&fd);
    return fd.best;
}

int GdiCaptureGameWindow(const wchar_t* path)
{
    HWND wnd = FindGameWindow();
    if (!wnd)
    {
        Log(L"GDI capture: game window not found");
        return 0;
    }

    RECT rc;
    GetClientRect(wnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return 0;

    HDC wndDc = GetDC(wnd);
    HDC memDc = CreateCompatibleDC(wndDc);
    HBITMAP bmp = CreateCompatibleBitmap(wndDc, w, h);
    HGDIOBJ old = SelectObject(memDc, bmp);

    // PrintWindow with PW_RENDERFULLCONTENT captures DirectX content on Windows 8.1+; plain BitBlt otherwise.
    if (!PrintWindow(wnd, memDc, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
        BitBlt(memDc, 0, 0, w, h, wndDc, 0, 0, SRCCOPY);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    unsigned char* pixels = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
    int ok = 0;
    SelectObject(memDc, old);
    if (pixels && GetDIBits(memDc, bmp, 0, (UINT)h, pixels, &bi, DIB_RGB_COLORS))
        ok = WritePngBgra(path, pixels, w, h, w * 4);

    free(pixels);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(wnd, wndDc);

    Log(ok ? L"Saved screenshot %s (%dx%d, GDI fallback)" : L"GDI capture failed for %s", path, w, h);
    return ok;
}
