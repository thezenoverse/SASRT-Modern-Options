// Extra UI strings. The game sets a TEXT element from its string table with
// SetLocalisedText(0x493DE0)(elementHash, textHash, arg). We hook it: if textHash is one of ours,
// the element gets a literal text object instead (the same way menus show literal rows).
#include "game.h"

#define ADDR_SET_LOCALISED_TEXT 0x00493DE0
#define MAX_STRINGS 160

typedef struct Entry
{
    unsigned int hash;
    GameString* object;
} Entry;

static Entry s_entries[MAX_STRINGS];
static int s_count = 0;
static BYTE* s_trampoline = NULL;

typedef char (__thiscall* SetLocalisedText_t)(void* owner, unsigned int elementHash, unsigned int textHash, int arg);

GameString* StringsMakeObject(const char* text, int capacity)
{
    GameString* obj = (GameString*)calloc(1, sizeof(GameString));
    if (!obj)
        return NULL;
    obj->vtable = GAMESTRING_VTABLE;
    obj->refCount = 0x10000000; // never reaches zero, so the game never frees our memory
    obj->capacity = capacity;
    obj->text = (char*)calloc(1, (size_t)capacity);
    StringsSetObject(obj, text);
    return obj;
}

void StringsSetObject(GameString* obj, const char* text)
{
    if (!obj || !obj->text)
        return;
    strncpy(obj->text, text ? text : "", (size_t)obj->capacity - 1);
    obj->text[obj->capacity - 1] = 0;
}

void StringsAdd(const char* key, const char* text)
{
    if (s_count >= MAX_STRINGS)
        return;
    s_entries[s_count].hash = HashString(key);
    s_entries[s_count].object = StringsMakeObject(text, 64);
    s_count++;
}

static GameString* Find(unsigned int hash);

void StringsSetText(const char* key, const char* text)
{
    GameString* obj = Find(HashString(key));
    if (obj)
        StringsSetObject(obj, text);
}

static GameString* Find(unsigned int hash)
{
    for (int i = 0; i < s_count; i++)
        if (s_entries[i].hash == hash)
            return s_entries[i].object;
    return NULL;
}

static char __fastcall Hook_SetLocalisedText(void* owner, void* edx, unsigned int elementHash, unsigned int textHash, int arg)
{
    (void)edx;
    GameString* custom = Find(textHash);
    if (custom)
        return SetTextObject(owner, elementHash, custom);
    return ((SetLocalisedText_t)(void*)s_trampoline)(owner, elementHash, textHash, arg);
}

int StringsInstall(void)
{
    // mov eax, [esp+4] / push esi / push edi
    static const unsigned char prologue[6] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57 };
    if (!PatchExpect(ADDR_SET_LOCALISED_TEXT, prologue, sizeof(prologue)))
    {
        Log(L"Strings: unexpected code at 0x%08X; custom texts disabled", ADDR_SET_LOCALISED_TEXT);
        return 0;
    }

    s_trampoline = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_trampoline)
        return 0;
    memcpy(s_trampoline, prologue, sizeof(prologue));
    s_trampoline[6] = 0xE9;
    *(LONG*)(s_trampoline + 7) = (LONG)((ADDR_SET_LOCALISED_TEXT + 6) - ((DWORD)s_trampoline + 11));

    PatchRel32(ADDR_SET_LOCALISED_TEXT, 0xE9, (void*)Hook_SetLocalisedText, 6);
    Log(L"Strings: hooked SetLocalisedText");
    return 1;
}
