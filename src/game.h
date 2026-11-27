#pragma once
// Addresses and calling conventions in ASN_App_PcDx9_Final.exe (Steam build, fixed image base
// 0x400000, no ASLR). Every patch checks the bytes it expects before writing.
#include "common.h"

// ---- globals ----
#define G_GAME              (*(BYTE**)0x00E9A410)   // game/frontend manager
#define G_UI                (*(void**)0x00E9A3FC)   // UI system, vfunc 0x13C -> text owner
#define G_ALLOCATOR         (*(void***)0x00BC58C0)  // allocator object; its first field is alloc(size, align, 0, 0)
#define G_INPUT_PLAYER      (*(int*)0x00BC7B68)
#define G_FRONTEND_PAUSED   (*(BYTE*)0x00BE0458)
#define G_OPTIONS_ROW       (*(int*)0x00C567E0)     // cOptions selected row
#define G_AUDIO_ROW         (*(int*)0x00C57810)     // cOptionsAudio selected row

#define GAME_MENU()         (*(void**)(G_GAME + 0x1E84))   // vertical menu
#define GAME_HEADER()       (*(void**)(G_GAME + 0x1E88))   // title / sub header

// Vertical menu object layout (max 10 rows)
#define MENU_MAX_ROWS       10
#define MENU_PREFIX(m)      ((const char*)(m) + 0x0C)     // "VERTICAL_MENU"
#define MENU_ROW_VALUE(m,r) (((int*)((BYTE*)(m) + 0x6C))[r])

// Input bits returned by GetPressed
#define IN_UP       0x02
#define IN_DOWN     0x04
#define IN_LEFT     0x08
#define IN_RIGHT    0x10
#define IN_ACCEPT   0x20
#define IN_BACK     0x40

// Row types for MenuBuild rows {literal, text, type, value}
#define ROW_BUTTON  0
#define ROW_TOGGLE  1
#define ROW_CYCLE   3   // up to 4 named options; only drawn by VERTICAL_MENU_SMALL
#define ROW_SLIDER  4

// ---- functions ----
typedef unsigned int (__cdecl* HashString_t)(const char*);
#define HashString ((HashString_t)0x00493530)

typedef int (__cdecl* MenuBuild_t)(void* menu, ...);
#define MenuBuild ((MenuBuild_t)0x0074BA60)

typedef void (__thiscall* MenuUpdate_t)(void* menu);
#define MenuUpdate ((MenuUpdate_t)0x0074B5A0)

typedef void (__thiscall* MenuSelectRow_t)(void* menu, int row, int a, int down, int b, int c, int d);
#define MenuSelectRow ((MenuSelectRow_t)0x0074A490)

typedef void (__thiscall* MenuSetSlider_t)(void* menu, int row, int value);   // 0..100
#define MenuSetSlider ((MenuSetSlider_t)0x0074AA70)

typedef void (__thiscall* MenuSetToggle_t)(void* menu, int row, int value);
#define MenuSetToggle ((MenuSetToggle_t)0x0074ACC0)

// ROW_CYCLE: option names (string-table hashes) and the shown option.
typedef void (__cdecl* MenuSetCycleNames_t)(void* menu, int row, int count, ...);
#define MenuSetCycleNames ((MenuSetCycleNames_t)0x0074C8F0)
typedef void (__thiscall* MenuSetCycle_t)(void* menu, int row, int value, int animate);
#define MenuSetCycle ((MenuSetCycle_t)0x0074ACE0)

// Looks up a UI element by path hash (returns NULL if the loaded scenes don't have it).
typedef void* (__thiscall* FindTextElement_t)(void* registry, unsigned int hash);
#define FindTextElement ((FindTextElement_t)0x00497250)
#define UI_REGISTRY ((void*)0x00EC1F2C)

typedef unsigned int (__thiscall* GetPressed_t)(void* game, int player);
#define GetPressed ((GetPressed_t)0x0068CF30)

typedef void (__cdecl* PlaySound_t)(int id, int arg);
#define PlayUiSound ((PlaySound_t)0x00721870)
#define PlayUiSound2 ((PlaySound_t)0x007218A0)

typedef void (__thiscall* SceneLeave_t)(void* scene);
#define SceneLeave ((SceneLeave_t)0x007218D0)

typedef void (__thiscall* PushScene_t)(void* game, const char* name, void* scene, int mode);
#define PushScene ((PushScene_t)0x00695690)

typedef void* (__cdecl* GetAllocator_t)(void);
#define GetAllocator ((GetAllocator_t)0x00602D50)

typedef void (__thiscall* Ctor_t)(void* self);
#define AudioOptionsCtor ((Ctor_t)0x008DE980)
#define NAME_COPTIONSAUDIO ((const char*)0x00A39E74)

typedef char (__thiscall* SetTextObject_t)(void* owner, unsigned int elementHash, void* textObject);
#define SetTextObject ((SetTextObject_t)0x00493D40)

// Text object the UI binds to TEXT elements (same layout the menu allocates).
typedef struct GameString
{
    void* vtable;      // 0x9F99BC
    int unk4;
    int refCount;
    int capacity;
    char* text;
} GameString;
#define GAMESTRING_VTABLE ((void*)0x009F99BC)

static inline void* UiTextOwner(void)
{
    typedef void* (__thiscall* Fn)(void*);
    void* ui = G_UI;
    return ((Fn)((*(void***)ui)[0x13C / 4]))(ui);
}

static inline void* GameAlloc(int size)
{
    typedef void* (__cdecl* AllocFn)(int, int, int, int);
    if (!G_ALLOCATOR)
        G_ALLOCATOR = (void**)GetAllocator();
    return ((AllocFn)G_ALLOCATOR[0])(size, 4, 0, 0);
}

// patch helpers (menu.c)
int PatchExpect(DWORD addr, const void* bytes, size_t len);
void PatchWrite(DWORD addr, const void* bytes, size_t len);
void PatchRel32(DWORD addr, unsigned char opcode, const void* target, size_t padTo);
int PatchVtableSlot(DWORD slotAddr, DWORD expected, void* hook);

// strings.c - extra UI strings looked up by hash, like the game's own string table.
int StringsInstall(void);
void StringsAdd(const char* key, const char* text);
void StringsSetText(const char* key, const char* text);
GameString* StringsMakeObject(const char* text, int capacity);
void StringsSetObject(GameString* obj, const char* text);

// settings.c - Documents\SART\settings.xml (what Launcher.exe writes, the game reads at start-up)
typedef struct VideoSettings
{
    int displayMode;   // 0 windowed, 1 fullscreen, 2 borderless (fullscreen window at desktop size)
    int width, height, refresh;
    int vsync, antialias, ambientOcclusion, motionBlur, screenSplash; // 0/1
    int water, texture, model, shadow;                                // 0 low, 1 normal, 2 high
    int graphicLevel;  // launcher preset: 0 low, 1 normal, 2 high, 3 custom
} VideoSettings;

typedef struct Resolution { int width, height, refresh; } Resolution;

int SettingsLoad(VideoSettings* out);
int SettingsSave(const VideoSettings* in);
int SettingsResolutions(const Resolution** list);   // count; enumerated once from D3D9

// live.c - apply settings while the game runs
int LiveInstall(void);
void LiveApply(const VideoSettings* s);

// pause.c - DISPLAY OPTIONS in the race pause menu
int PauseMenuInstall(void);
void PauseMenuPoll(void);

// layout.c - Controller Options / Display Options use normal-size rows again
void LayoutFixApply(void);
int LayoutFixInstall(void);
void LayoutStepControllerPos(int dir);
#define COMPACT_CONTROLLER 0
#define COMPACT_WIDE 1
void LayoutCompactFix(int mode);
void LayoutRestoreDefinitions(void);
void PortBegin(const int* types, int count);
void PortEnsureScene(void);
void PortKeepScene(void);
int PortAfterBuild(const int* types, int count);   // 0: compact layout not loaded yet
void PortEnd(void);
void PortLeave(void);
void PortShowSelector(int row, int show);
void PortFinishSelector(int row);
void PortTick(void);
void PortTrace(int frames);
void PortSetFrameCallback(void (*fn)(void));
extern volatile int g_normalToggles;

// uidump.c
int UiDumpElements(const wchar_t* path);

// video.c
int VideoMenuInstall(void);
void VideoMenuOpen(void);   // called when VIDEO OPTIONS is chosen in cOptions
extern volatile int g_videoActive;
