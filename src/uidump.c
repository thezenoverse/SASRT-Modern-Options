// F8: dumps the raw memory of menu text elements from both menu layouts side by side
// (VERTICAL_MENU vs VERTICAL_MENU_SMALL), to find which values make the compact layout's
// buttons bigger on PC. Read-only; nothing in the game is changed.
#include "game.h"

static const char* s_names[] =
{
    "VERTICAL_MENU\\TEXT_1",        "VERTICAL_MENU_SMALL\\TEXT_1",
    "VERTICAL_MENU\\TEXT_2",        "VERTICAL_MENU_SMALL\\TEXT_2",
    "VERTICAL_MENU\\TEXT_3",        "VERTICAL_MENU_SMALL\\TEXT_3",
    "VERTICAL_MENU\\SILDER_TEXT_1", "VERTICAL_MENU_SMALL\\SILDER_TEXT_1",
    "VERTICAL_MENU_SMALL\\TOGGLE_TEXT_1_1", "VERTICAL_MENU_SMALL\\TOGGLE_TEXT_2_1",
};

#define DUMP_BYTES 0x140

static int Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    return (const BYTE*)p + n <= (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
}

static void Out(HANDLE f, const char* fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 1, fmt, a);
    va_end(a);
    if (n < 0) n = sizeof(buf) - 1;
    DWORD w;
    WriteFile(f, buf, (DWORD)n, &w, NULL);
}

// The registry (0xEC1F2C) is a binary tree embedded in each element at +0x04:
//   element+0x08 = left, element+0x0C = right, element+0x14 = name hash,
//   element+0xE0 -> type object, type+4 = 'TEXT' / 'TXTR' four-cc.
static int s_walked;

static void Walk(HANDLE f, const BYTE* node, int depth)
{
    if (!node || depth > 64 || s_walked > 4000 || !Readable(node, 0x100))
        return;
    const BYTE* el = node - 4;
    if (!Readable(el, 0x140))
        return;
    Walk(f, *(const BYTE* const*)(node + 4), depth + 1);

    s_walked++;
    const BYTE* type = *(const BYTE* const*)(el + 0xE0);
    char tag[5] = "????";
    if (Readable(type, 8))
        memcpy(tag, type + 4, 4);
    const float* fl = (const float*)el;
    Out(f, "%08X %08X %.4s pos %.4f %.4f scale %.4f %.4f font %.1f | m %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f | e8 %08X 134 %08X\r\n",
        (unsigned)*(const DWORD*)(el + 0x14), (unsigned)(DWORD)el, tag,
        fl[0x3C / 4], fl[0x40 / 4], fl[0x44 / 4], fl[0x48 / 4], fl[0x6C / 4],
        fl[0x104 / 4], fl[0x108 / 4], fl[0x10C / 4], fl[0x110 / 4], fl[0x114 / 4], fl[0x118 / 4], fl[0x11C / 4], fl[0x120 / 4],
        (unsigned)*(const DWORD*)(el + 0xE8), (unsigned)*(const DWORD*)(el + 0x134));

    Walk(f, *(const BYTE* const*)(node + 8), depth + 1);
}

// Collects direct children of `parent` (element+0xE8 == parent) while walking the registry.
static const BYTE* s_kids[64];
static int s_kidCount;
static void CollectKids(const BYTE* node, const BYTE* parent, int depth)
{
    if (!node || depth > 64 || !Readable(node, 0x100))
        return;
    const BYTE* el = node - 4;
    CollectKids(*(const BYTE* const*)(node + 4), parent, depth + 1);
    if (Readable(el, 0xF0) && *(const BYTE* const*)(el + 0xE8) == parent && s_kidCount < 64)
        s_kids[s_kidCount++] = el;
    CollectKids(*(const BYTE* const*)(node + 8), parent, depth + 1);
}

static void Raw(HANDLE f, const char* title, const BYTE* p, int bytes)
{
    Out(f, "\r\n--- %s @%08X ---\r\n", title, (unsigned)(DWORD)p);
    if (!Readable(p, (size_t)bytes))
    {
        Out(f, "(unreadable)\r\n");
        return;
    }
    for (int o = 0; o < bytes; o += 4)
        Out(f, "+%03X  %08X  %14.4f\r\n", o, (unsigned)*(const DWORD*)(p + o), *(const float*)(p + o));
}

// Row 1 of whichever layout is loaded: every child element in full, plus the objects a
// TEXT/TXTR element points at (+0xE0 type/style, +0xE4 resource), to find what sets sizes.
static void DumpRow1(HANDLE f, const char* rowName)
{
    const BYTE* row = NULL;
    // Find the row group by hash among all elements.
    unsigned int hash = HashString(rowName);
    const BYTE* stack[256];
    int sp = 0;
    const BYTE* root = *(const BYTE* const*)UI_REGISTRY;
    if (root) stack[sp++] = root;
    while (sp > 0 && !row)
    {
        const BYTE* node = stack[--sp];
        if (!Readable(node, 0x100))
            continue;
        if (*(const DWORD*)(node + 0x10) == hash)
            row = node - 4;
        const BYTE* l = *(const BYTE* const*)(node + 4);
        const BYTE* r = *(const BYTE* const*)(node + 8);
        if (l && sp < 255) stack[sp++] = l;
        if (r && sp < 255) stack[sp++] = r;
    }
    if (!row)
        return;

    Out(f, "\r\n######## %s ########\r\n", rowName);
    Raw(f, rowName, row, 0x200);
    s_kidCount = 0;
    CollectKids(root, row, 0);
    for (int i = 0; i < s_kidCount; i++)
    {
        const BYTE* k = s_kids[i];
        char title[96];
        _snprintf(title, sizeof(title), "child %08X", (unsigned)*(const DWORD*)(k + 0x14));
        title[sizeof(title) - 1] = 0;
        Raw(f, title, k, 0x200);
        const BYTE* type = *(const BYTE* const*)(k + 0xE0);
        if (Readable(type, 8) && (!memcmp(type + 4, "TEXT", 4) || !memcmp(type + 4, "TXTR", 4)))
        {
            Raw(f, "  +E0 object", type, 0x100);
            Raw(f, "  +E4 object", *(const BYTE* const*)(k + 0xE4), 0x100);
        }
    }
}

int UiDumpElements(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;

    int count = (int)(sizeof(s_names) / sizeof(s_names[0]));
    const BYTE* el[16] = { 0 };
    for (int i = 0; i < count; i++)
    {
        el[i] = (const BYTE*)FindTextElement(UI_REGISTRY, HashString(s_names[i]));
        Out(f, "%-40s %08X\r\n", s_names[i], (unsigned)(DWORD)el[i]);
    }

    // Raw dump of every element that is loaded right now. Only one layout is loaded at a time
    // (building a menu with the other layout unloads it), so press F8 once on Audio Options and
    // once on Controller Options and compare the two files.
    for (int i = 0; i < count; i++)
    {
        const BYTE* a = el[i];
        if (!a || !Readable(a, DUMP_BYTES))
            continue;
        Out(f, "\r\n=== %s ===\r\n", s_names[i]);
        for (int o = 0; o < DUMP_BYTES; o += 4)
            Out(f, "+%03X  %08X  %14.4f\r\n", o, (unsigned)*(const DWORD*)(a + o), *(const float*)(a + o));
    }

    DumpRow1(f, "VERTICAL_MENU\\1");
    DumpRow1(f, "VERTICAL_MENU_SMALL\\1");

    Out(f, "\r\n=== registry (hash, element, type, local pos, scale, font, world matrix) ===\r\n");
    s_walked = 0;
    Walk(f, *(const BYTE* const*)UI_REGISTRY, 0);
    Out(f, "(%d elements)\r\n", s_walked);

    CloseHandle(f);
    return 1;
}
