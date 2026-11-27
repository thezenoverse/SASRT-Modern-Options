// Records every file the game opens (path + time) so that, after opening a menu and pressing F5,
// the dump shows which data files that menu loaded. Hooks the executable's CreateFileA/W imports,
// chaining to whatever is already there (Sumo Engine Mod Loader's redirection hooks).
#include "common.h"

typedef HANDLE (WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static CreateFileW_t s_nextW = NULL;
static CreateFileA_t s_nextA = NULL;

#define LOG_CAPACITY 16384

typedef struct Entry
{
    DWORD tick;
    DWORD access;
    BOOL ok;
    wchar_t* path;
} Entry;

static Entry s_entries[LOG_CAPACITY];
static unsigned int s_head = 0;      // total entries ever written
static unsigned int s_lastDump = 0;  // value of s_head at the previous dump
static CRITICAL_SECTION s_lock;
static DWORD s_startTick;

static void Record(const wchar_t* path, DWORD access, BOOL ok)
{
    if (!path)
        return;
    wchar_t* copy = _wcsdup(path);
    EnterCriticalSection(&s_lock);
    Entry* e = &s_entries[s_head % LOG_CAPACITY];
    free(e->path);
    e->path = copy;
    e->tick = GetTickCount();
    e->access = access;
    e->ok = ok;
    s_head++;
    LeaveCriticalSection(&s_lock);
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disposition, DWORD flags, HANDLE templ)
{
    HANDLE h = s_nextW(name, access, share, sa, disposition, flags, templ);
    DWORD err = GetLastError();
    Record(name, access, h != INVALID_HANDLE_VALUE);
    SetLastError(err);
    return h;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disposition, DWORD flags, HANDLE templ)
{
    HANDLE h = s_nextA(name, access, share, sa, disposition, flags, templ);
    DWORD err = GetLastError();
    if (name)
    {
        wchar_t wide[MAX_PATH * 2];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2))
            Record(wide, access, h != INVALID_HANDLE_VALUE);
    }
    SetLastError(err);
    return h;
}

static int PatchImport(HMODULE module, const char* name, void* hook, void** next)
{
    BYTE* base = (BYTE*)module;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;

    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
    {
        if (!imp->OriginalFirstThunk)
            continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (int n = 0; names[n].u1.AddressOfData; n++)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names[n].u1.Ordinal))
                continue;
            const char* importName = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names[n].u1.AddressOfData))->Name;
            if (strcmp(importName, name) != 0)
                continue;

            void** slot = (void**)&iat[n].u1.Function;
            if (*slot == hook)
                continue;
            DWORD old;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
            {
                if (!*next)
                    *next = *slot; // whatever is there now (loader hook or kernel32)
                *slot = hook;
                VirtualProtect(slot, sizeof(void*), old, &old);
                patched++;
            }
        }
    }
    return patched;
}

void FileLogInstall(void)
{
    InitializeCriticalSection(&s_lock);
    s_startTick = GetTickCount();

    HMODULE exe = GetModuleHandleW(NULL);
    int w = PatchImport(exe, "CreateFileW", (void*)Hook_CreateFileW, (void**)&s_nextW);
    int a = PatchImport(exe, "CreateFileA", (void*)Hook_CreateFileA, (void**)&s_nextA);

    // Make sure both "next" pointers are valid even if the exe only imports one of them.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!s_nextW) s_nextW = (CreateFileW_t)(void*)GetProcAddress(k32, "CreateFileW");
    if (!s_nextA) s_nextA = (CreateFileA_t)(void*)GetProcAddress(k32, "CreateFileA");

    Log(L"File log: hooked %d CreateFileW and %d CreateFileA import(s)", w, a);
}

int FileLogDump(const wchar_t* path)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return -1;

    static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD written;
    WriteFile(file, bom, 3, &written, NULL);

    EnterCriticalSection(&s_lock);
    unsigned int from = s_lastDump;
    if (s_head - from > LOG_CAPACITY)
        from = s_head - LOG_CAPACITY;

    char header[256];
    int hl = _snprintf(header, sizeof(header),
        "# Files opened since the previous F5 (%u entries%s)\r\n# time_ms  result  access  path\r\n",
        s_head - from, (s_lastDump < from) ? ", oldest entries dropped" : "");
    WriteFile(file, header, (DWORD)hl, &written, NULL);

    int count = 0;
    for (unsigned int i = from; i < s_head; i++)
    {
        Entry* e = &s_entries[i % LOG_CAPACITY];
        wchar_t line[MAX_PATH * 2 + 64];
        int len = _snwprintf(line, MAX_PATH * 2 + 63, L"%9lu  %-6s  %-5s  %s\r\n",
            (unsigned long)(e->tick - s_startTick), e->ok ? L"ok" : L"FAIL",
            (e->access & GENERIC_WRITE) ? L"write" : L"read", e->path ? e->path : L"");
        if (len < 0)
            continue;
        char utf8[(MAX_PATH * 2 + 64) * 3];
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8), NULL, NULL);
        if (bytes > 0)
            WriteFile(file, utf8, (DWORD)bytes, &written, NULL);
        count++;
    }
    s_lastDump = s_head;
    LeaveCriticalSection(&s_lock);

    CloseHandle(file);
    return count;
}
