// Reads and writes Documents\SART\settings.xml, the file Launcher.exe writes and the game reads
// once at start-up (0x416C50). Keys used by the game:
//   screenmode: width, height, bpp, refresh, fullscreen (0 windowed, 1 fullscreen, 2 borderless)
//   setup:      graphiclevel (launcher preset 0 low / 1 normal / 2 high / 3 custom),
//               waterlevel, texturelevel, modellevel, shadowlevel (0..2)
//   options:    ambientocclusion, antialias, motionblur, screensplash, vsync (0/1)
// Values are edited in place (element text or attribute), so everything else in the file,
// including anything this mod doesn't know about, is kept exactly as the launcher wrote it.
#define COBJMACROS
#define CINTERFACE
#include "game.h"
#include <shlobj.h>
#include <d3d9.h>

static int SettingsPath(wchar_t* out, size_t len)
{
    wchar_t docs[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs)))
        return 0;
    _snwprintf(out, len, L"%s\\SART\\settings.xml", docs);
    out[len - 1] = 0;
    return 1;
}

static char* ReadAll(const wchar_t* path, DWORD* size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD n = GetFileSize(f, NULL), read = 0;
    char* buf = (n != INVALID_FILE_SIZE && n < 4 * 1024 * 1024) ? (char*)malloc(n + 1) : NULL;
    if (buf && ReadFile(f, buf, n, &read, NULL))
    {
        buf[read] = 0;
        *size = read;
    }
    else
    {
        free(buf);
        buf = NULL;
    }
    CloseHandle(f);
    return buf;
}

static int IsNameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':';
}

// Finds the value of `key` as <key>value</key> or key="value". Returns 1 and the value's span.
static int FindValue(const char* xml, const char* key, size_t* start, size_t* end)
{
    size_t klen = strlen(key);
    for (const char* p = strstr(xml, key); p; p = strstr(p + 1, key))
    {
        char before = p > xml ? p[-1] : 0;
        char after = p[klen];
        if (IsNameChar(before) || IsNameChar(after))
            continue;

        if (before == '<')
        {
            const char* gt = strchr(p, '>');
            if (!gt || gt[-1] == '/')
                continue;
            const char* close = strstr(gt + 1, "</");
            if (!close)
                continue;
            const char* v = gt + 1;
            while (v < close && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
            const char* e = close;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
            *start = (size_t)(v - xml);
            *end = (size_t)(e - xml);
            return 1;
        }

        const char* q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=')
            continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"' && *q != '\'')
            continue;
        char quote = *q++;
        const char* e = strchr(q, quote);
        if (!e)
            continue;
        *start = (size_t)(q - xml);
        *end = (size_t)(e - xml);
        return 1;
    }
    return 0;
}

static int GetInt(const char* xml, const char* key, int fallback)
{
    size_t s, e;
    if (!FindValue(xml, key, &s, &e))
        return fallback;
    char tmp[32];
    size_t n = e - s < sizeof(tmp) - 1 ? e - s : sizeof(tmp) - 1;
    memcpy(tmp, xml + s, n);
    tmp[n] = 0;
    return (int)(atof(tmp) + 0.5);
}

int SettingsLoad(VideoSettings* out)
{
    // The game's own copy (0xEEC9C8) holds what it started with; use it as the fallback.
    const int* live = (const int*)0x00EEC9C8;
    out->width = live[0];
    out->height = live[1];
    out->refresh = live[3];
    out->displayMode = live[4];
    out->water = live[9];
    out->texture = live[10];
    out->model = live[11];
    out->shadow = live[12];
    out->ambientOcclusion = live[13];
    out->antialias = live[14];
    out->motionBlur = live[15];
    out->screenSplash = live[16];
    out->vsync = live[17];
    out->graphicLevel = 3;

    wchar_t path[MAX_PATH];
    DWORD size = 0;
    char* xml = SettingsPath(path, MAX_PATH) ? ReadAll(path, &size) : NULL;
    if (!xml)
    {
        Log(L"Settings: could not read settings.xml; using the values the game started with");
        return 0;
    }

    out->width = GetInt(xml, "width", out->width);
    out->height = GetInt(xml, "height", out->height);
    out->refresh = GetInt(xml, "refresh", out->refresh);
    out->displayMode = GetInt(xml, "fullscreen", out->displayMode);
    out->water = GetInt(xml, "waterlevel", out->water);
    out->texture = GetInt(xml, "texturelevel", out->texture);
    out->model = GetInt(xml, "modellevel", out->model);
    out->shadow = GetInt(xml, "shadowlevel", out->shadow);
    out->ambientOcclusion = GetInt(xml, "ambientocclusion", out->ambientOcclusion);
    out->antialias = GetInt(xml, "antialias", out->antialias);
    out->motionBlur = GetInt(xml, "motionblur", out->motionBlur);
    out->screenSplash = GetInt(xml, "screensplash", out->screenSplash);
    out->vsync = GetInt(xml, "vsync", out->vsync);
    out->graphicLevel = GetInt(xml, "graphiclevel", out->graphicLevel);
    free(xml);

    Log(L"Settings: %dx%d@%d mode=%d vsync=%d aa=%d ao=%d mb=%d splash=%d water=%d tex=%d model=%d shadow=%d",
        out->width, out->height, out->refresh, out->displayMode, out->vsync, out->antialias, out->ambientOcclusion,
        out->motionBlur, out->screenSplash, out->water, out->texture, out->model, out->shadow);
    return 1;
}

// Replaces one value; keeps the number style that was there (e.g. "1.000000").
static char* SetInt(char* xml, const char* key, int value, int* missing)
{
    size_t s, e;
    if (!FindValue(xml, key, &s, &e))
    {
        (*missing)++;
        Log(L"Settings: key '%hs' not found in settings.xml", key);
        return xml;
    }

    char repl[32];
    const char* dot = memchr(xml + s, '.', e - s);
    if (dot)
    {
        int decimals = (int)(e - (size_t)(dot - xml) - 1);
        _snprintf(repl, sizeof(repl), "%.*f", decimals, (double)value);
    }
    else
        _snprintf(repl, sizeof(repl), "%d", value);
    repl[sizeof(repl) - 1] = 0;

    size_t oldLen = strlen(xml), rlen = strlen(repl);
    char* out = (char*)malloc(oldLen - (e - s) + rlen + 1);
    if (!out)
        return xml;
    memcpy(out, xml, s);
    memcpy(out + s, repl, rlen);
    strcpy(out + s + rlen, xml + e);
    free(xml);
    return out;
}

int SettingsSave(const VideoSettings* in)
{
    wchar_t path[MAX_PATH];
    DWORD size = 0;
    char* xml = SettingsPath(path, MAX_PATH) ? ReadAll(path, &size) : NULL;
    if (!xml)
    {
        Log(L"Settings: settings.xml not found - run Launcher.exe once so it creates the file");
        return 0;
    }
    if (size >= 2 && (unsigned char)xml[0] == 0xFF && (unsigned char)xml[1] == 0xFE)
    {
        Log(L"Settings: settings.xml is UTF-16, which this build can't edit yet");
        free(xml);
        return 0;
    }

    // Keep the launcher's preset selector honest: a preset only if all four levels match it.
    int preset = (in->water == in->texture && in->water == in->model && in->water == in->shadow) ? in->water : 3;

    int missing = 0;
    xml = SetInt(xml, "graphiclevel", preset, &missing);
    xml = SetInt(xml, "width", in->width, &missing);
    xml = SetInt(xml, "height", in->height, &missing);
    xml = SetInt(xml, "refresh", in->refresh, &missing);
    xml = SetInt(xml, "fullscreen", in->displayMode, &missing);
    xml = SetInt(xml, "waterlevel", in->water, &missing);
    xml = SetInt(xml, "texturelevel", in->texture, &missing);
    xml = SetInt(xml, "modellevel", in->model, &missing);
    xml = SetInt(xml, "shadowlevel", in->shadow, &missing);
    xml = SetInt(xml, "ambientocclusion", in->ambientOcclusion, &missing);
    xml = SetInt(xml, "antialias", in->antialias, &missing);
    xml = SetInt(xml, "motionblur", in->motionBlur, &missing);
    xml = SetInt(xml, "screensplash", in->screenSplash, &missing);
    xml = SetInt(xml, "vsync", in->vsync, &missing);

    // Write to a temp file first so a failed write never leaves a half-written settings.xml.
    wchar_t tmp[MAX_PATH];
    _snwprintf(tmp, MAX_PATH, L"%s.tmp", path);
    tmp[MAX_PATH - 1] = 0;
    int ok = 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0, len = (DWORD)strlen(xml);
        ok = WriteFile(f, xml, len, &written, NULL) && written == len;
        CloseHandle(f);
        ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    }
    free(xml);

    Log(ok ? L"Settings: saved settings.xml (%d key(s) missing)" : L"Settings: failed to save settings.xml", missing);
    return ok;
}

static Resolution* s_res = NULL;
static int s_resCount = -1;

static int CompareRes(const void* a, const void* b)
{
    const Resolution* x = (const Resolution*)a;
    const Resolution* y = (const Resolution*)b;
    if (x->width != y->width) return x->width - y->width;
    return x->height - y->height;
}

int SettingsResolutions(const Resolution** list)
{
    if (s_resCount < 0)
    {
        s_resCount = 0;
        typedef IDirect3D9* (WINAPI* Create_t)(UINT);
        HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
        Create_t create = d3d9 ? (Create_t)(void*)GetProcAddress(d3d9, "Direct3DCreate9") : NULL;
        IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : NULL;
        if (d3d)
        {
            UINT n = IDirect3D9_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
            s_res = (Resolution*)calloc(n ? n : 1, sizeof(Resolution));
            for (UINT i = 0; s_res && i < n; i++)
            {
                D3DDISPLAYMODE m;
                if (FAILED(IDirect3D9_EnumAdapterModes(d3d, D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m)))
                    continue;
                if (m.Width < 640 || m.Height < 480)
                    continue;
                int found = 0;
                for (int k = 0; k < s_resCount; k++)
                {
                    if (s_res[k].width == (int)m.Width && s_res[k].height == (int)m.Height)
                    {
                        if ((int)m.RefreshRate > s_res[k].refresh)
                            s_res[k].refresh = (int)m.RefreshRate; // keep the highest rate per size
                        found = 1;
                        break;
                    }
                }
                if (!found)
                {
                    s_res[s_resCount].width = (int)m.Width;
                    s_res[s_resCount].height = (int)m.Height;
                    s_res[s_resCount].refresh = (int)m.RefreshRate;
                    s_resCount++;
                }
            }
            IDirect3D9_Release(d3d);
            if (s_resCount)
                qsort(s_res, (size_t)s_resCount, sizeof(Resolution), CompareRes);
        }
        Log(L"Settings: %d screen resolution(s) available", s_resCount);
    }
    *list = s_res;
    return s_resCount;
}
