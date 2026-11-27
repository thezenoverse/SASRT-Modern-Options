#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MO_NAME L"Modern Options"
#define MO_VERSION L"1.4.0"

// Passed in by Sumo Engine Mod Loader to Init/PostInit (see the SumoModLoader README).
typedef struct SumoModInfo
{
    int Size;
    int Version;
    const wchar_t* ModDirectory;
    const wchar_t* GameDirectory;
    const wchar_t* ModsDbPath;
} SumoModInfo;

// main.c
extern wchar_t g_modDirectory[MAX_PATH];
extern wchar_t g_dumpDirectory[MAX_PATH];
void Log(const wchar_t* fmt, ...);
// Builds "<Dumps>\<prefix>_<YYYYMMDD_HHMMSS_mmm>.<ext>" from one shared timestamp per F5 press.
void DumpPath(wchar_t* out, size_t outLen, const wchar_t* stamp, const wchar_t* prefix, const wchar_t* ext);

// png.c - writes 32-bit BGRA pixels (top-down) as an RGB PNG.
int WritePngBgra(const wchar_t* path, const unsigned char* bgra, int width, int height, int stride);

// d3d9hook.c
int D3D9HookInstall(void);
int D3D9HookActive(void);            // a hooked Present ran within the last second
void D3D9RequestCapture(const wchar_t* path);

// gdicapture.c - fallback when the D3D9 hook isn't running (e.g. D3D9Ex).
int GdiCaptureGameWindow(const wchar_t* path);
HWND FindGameWindow(void);

// menu.c - VIDEO OPTIONS row in the Options menu.
int MenuPatchInstall(void);

// filelog.c - records every file the game opens, dumped on F5.
void FileLogInstall(void);
int FileLogDump(const wchar_t* path); // writes entries since the last dump, returns count
