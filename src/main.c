// SASRT Modern Options - goal: put the launcher's settings (resolution, AA, controls, ...) into the
// game's own Options menu.
//
// Research helpers (debug builds only, MO_DEBUG_TOOLS): press F5 in-game to write into "<mod>\Dumps\":
//   screen_<time>.png  - the current frame (D3D9 back buffer; GDI fallback)
//   files_<time>.txt   - every file the game opened since the previous F5
// F8 dumps the UI elements, F6 / F7 move the Controller Options menu entry.
// Release builds only write "<mod>\ModernOptions.log".
#include "game.h"

wchar_t g_modDirectory[MAX_PATH];
wchar_t g_dumpDirectory[MAX_PATH];

static HANDLE s_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION s_logLock;

void Log(const wchar_t* fmt, ...)
{
    if (s_logFile == INVALID_HANDLE_VALUE)
        return;

    wchar_t buffer[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int prefix = _snwprintf(buffer, 64, L"[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    int len = _vsnwprintf(buffer + prefix, 1021 - prefix, fmt, args);
    va_end(args);
    if (len < 0)
        len = 1021 - prefix;
    len += prefix;
    buffer[len++] = L'\r';
    buffer[len++] = L'\n';

    char utf8[1024 * 3];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, len, utf8, sizeof(utf8), NULL, NULL);
    EnterCriticalSection(&s_logLock);
    DWORD written;
    if (bytes > 0)
        WriteFile(s_logFile, utf8, (DWORD)bytes, &written, NULL);
    LeaveCriticalSection(&s_logLock);
}

void DumpPath(wchar_t* out, size_t outLen, const wchar_t* stamp, const wchar_t* prefix, const wchar_t* ext)
{
    _snwprintf(out, outLen, L"%s\\%s_%s.%s", g_dumpDirectory, prefix, stamp, ext);
    out[outLen - 1] = 0;
}

// Writes the game executable as it sits in memory (after Steam's DRM wrapper has decrypted it)
// to "<Dumps>\\ASN_App_PcDx9_Final.dumped.exe", laid out so disassemblers can load it directly
// (section raw offsets = virtual addresses). Done once; delete the file to dump again.
static void __attribute__((unused)) DumpExeImage(void)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\ASN_App_PcDx9_Final.dumped.exe", g_dumpDirectory);
    path[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return;

    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;

    BYTE* copy = (BYTE*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!copy)
        return;

    // Copy page by page, skipping anything not readable.
    for (DWORD off = 0; off < size;)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(base + off, &mbi, sizeof(mbi)))
            break;
        DWORD regionEnd = (DWORD)((BYTE*)mbi.BaseAddress + mbi.RegionSize - base);
        if (regionEnd > size)
            regionEnd = size;
        DWORD prot = mbi.Protect & 0xFF;
        int readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && prot != PAGE_NOACCESS;
        if (readable)
            memcpy(copy + off, base + off, regionEnd - off);
        off = regionEnd;
    }

    IMAGE_NT_HEADERS* cnt = (IMAGE_NT_HEADERS*)(copy + ((IMAGE_DOS_HEADER*)copy)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(cnt);
    for (WORD i = 0; i < cnt->FileHeader.NumberOfSections; i++)
    {
        sec[i].PointerToRawData = sec[i].VirtualAddress;
        sec[i].SizeOfRawData = sec[i].Misc.VirtualSize;
    }
    cnt->OptionalHeader.FileAlignment = cnt->OptionalHeader.SectionAlignment;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, copy, size, &written, NULL);
        CloseHandle(file);
        Log(L"Dumped in-memory executable (%lu bytes) to %s", (unsigned long)written, path);
    }
    VirtualFree(copy, 0, MEM_RELEASE);
}

static int GameHasFocus(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg)
        GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void OnDumpKey(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    _snwprintf(stamp, 64, L"%04d%02d%02d_%02d%02d%02d_%03d", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    CreateDirectoryW(g_dumpDirectory, NULL);

    wchar_t filesPath[MAX_PATH];
    DumpPath(filesPath, MAX_PATH, stamp, L"files", L"txt");
    int files = FileLogDump(filesPath);
    Log(L"F5: wrote %d file entries to %s", files, filesPath);

    wchar_t screenPath[MAX_PATH];
    DumpPath(screenPath, MAX_PATH, stamp, L"screen", L"png");
    if (D3D9HookActive())
        D3D9RequestCapture(screenPath); // taken on the next Present
    else
        GdiCaptureGameWindow(screenPath);

    MessageBeep(MB_OK);
}

static DWORD WINAPI __attribute__((unused)) KeyThread(LPVOID param)
{
    (void)param;
    int was5 = 0, was6 = 0, was7 = 0, was8 = 0;
    for (;;)
    {
        int d5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        int d6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        int d7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        int d8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        int focus = GameHasFocus();
        if (d8 && !was8 && focus)
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t stamp[64], path[MAX_PATH];
            _snwprintf(stamp, 64, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            DumpPath(path, MAX_PATH, stamp, L"elements", L"txt");
            CreateDirectoryW(g_dumpDirectory, NULL);
            Log(UiDumpElements(path) ? L"F8: wrote %s" : L"F8: could not write %s", path);
            MessageBeep(MB_OK);
        }
        was8 = d8;
        if (d5 && !was5 && focus)
            OnDumpKey();
        if (d6 && !was6 && focus)
        {
            LayoutStepControllerPos(+1);
            MessageBeep(MB_OK);
        }
        if (d7 && !was7 && focus)
        {
            LayoutStepControllerPos(-1);
            MessageBeep(MB_OK);
        }
        was5 = d5;
        was6 = d6;
        was7 = d7;
        Sleep(30);
    }
    return 0;
}

// Logs the first few crashes (access violations) with the code address and the return addresses
// found on the stack, so a crash report can be matched to the game's code.
static volatile LONG s_crashes;
static BYTE* g_modBase;
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    // IsBadReadPtr / IsBadWritePtr fault on purpose inside KernelBase / kernel32: not a crash.
    HMODULE where = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &where) &&
        (where == GetModuleHandleW(L"kernelbase.dll") || where == GetModuleHandleW(L"kernel32.dll")))
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&s_crashes) > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD stack[8] = { 0 };
    int k = 0;
    DWORD* sp = (DWORD*)c->Esp;
    for (int j = 0; j < 256 && k < 8; j++)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + j, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            break;
        DWORD v = sp[j];
        if ((v > 0x401000 && v < 0x9B8000) || (v > (DWORD)g_modBase && v < (DWORD)g_modBase + 0x100000))
            stack[k++] = v;
    }
    Log(L"CRASH %08lX at %08lX (%s %08lX) eax %08lX ecx %08lX edx %08lX esi %08lX edi %08lX | stack %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX | mod base %p",
        code, (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress,
        ep->ExceptionRecord->ExceptionInformation[0] ? L"write" : L"read",
        (DWORD)ep->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi,
        stack[0], stack[1], stack[2], stack[3], stack[4], stack[5], stack[6], stack[7], g_modBase);
    return EXCEPTION_CONTINUE_SEARCH;
}

__declspec(dllexport) void Init(SumoModInfo* info)
{
    InitializeCriticalSection(&s_logLock);
    {
        HMODULE self = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void*)CrashLogger, &self);
        g_modBase = (BYTE*)self;
        AddVectoredExceptionHandler(0, CrashLogger);
    }

    if (info && info->Size >= (int)sizeof(SumoModInfo) && info->ModDirectory)
        wcsncpy(g_modDirectory, info->ModDirectory, MAX_PATH - 1);
    else
    {
        // Fallback: folder of this DLL.
        HMODULE self = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void*)Init, &self);
        GetModuleFileNameW(self, g_modDirectory, MAX_PATH);
        wchar_t* slash = wcsrchr(g_modDirectory, L'\\');
        if (slash) *slash = 0;
    }

    _snwprintf(g_dumpDirectory, MAX_PATH, L"%s\\Dumps", g_modDirectory);
    g_dumpDirectory[MAX_PATH - 1] = 0;

    wchar_t logPath[MAX_PATH];
#if MO_DEBUG_TOOLS
    CreateDirectoryW(g_dumpDirectory, NULL);
    _snwprintf(logPath, MAX_PATH, L"%s\\ModernOptions.log", g_dumpDirectory);
#else
    _snwprintf(logPath, MAX_PATH, L"%s\\ModernOptions.log", g_modDirectory);
#endif
    logPath[MAX_PATH - 1] = 0;
    s_logFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_logFile != INVALID_HANDLE_VALUE)
    {
        static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written;
        WriteFile(s_logFile, bom, 3, &written, NULL);
    }

    Log(L"%s %s loaded from %s", MO_NAME, MO_VERSION, g_modDirectory);
#if MO_DEBUG_TOOLS
    FileLogInstall();
#endif
    StringsInstall();
    if (MenuPatchInstall())
        VideoMenuInstall();
    LayoutFixInstall();
    LiveInstall();
    PauseMenuInstall();
}

__declspec(dllexport) void PostInit(SumoModInfo* info)
{
    (void)info;
#if MO_DEBUG_TOOLS
    DumpExeImage();
    D3D9HookInstall();

    HANDLE thread = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
    if (thread)
        CloseHandle(thread);
    Log(L"Debug tools on: F5 dumps the screen and the list of opened files, F8 the UI elements");
#endif
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
