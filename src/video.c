// The DISPLAY OPTIONS screen.
//
// It runs inside the game's own Audio Options scene class (cOptionsAudio): when the player picks
// VIDEO OPTIONS, the mod sets g_videoActive and lets the game open that scene. While the flag is
// set, the scene's menu rows, header and input handling are the mod's:
//
//   cOptionsAudio vtable 0xA3BA1C
//     [7]  0x8DF720  value set with the mouse (row, value)   -> Hook_ValueChanged
//     [8]  0x8DEF50  row clicked with the mouse                -> Hook_Clicked
//     [10] 0x8DF1E0  per-frame input                           -> Hook_Update
//     [13] 0x8DEE80  scene finished leaving -> back to Options -> Hook_Next
//   cOptionsAudio::Enter 0x8DEA50 (not replaced), two calls inside it are redirected:
//     0x8DEB23  MenuBuild (7 audio rows)     -> VideoMenuBuild
//     0x8DEBB7  header (title / sub header)  -> VideoHeader
//
// Row types (the game's own, see game.h):
//   on/off settings        ROW_TOGGLE
//   display mode, quality  ROW_CYCLE  - the 3-option selector
//   screen resolution      ROW_CYCLE  - same selector, three rotating name slots
// The screen uses the normal layout, like Audio Options. The selector only exists in the compact
// layout, so layout.c ports it into the normal rows (see "Selector port" there).
//
// The menu object has room for 10 rows (fixed arrays at +0x44/+0x6C/+0x94/+0xBC, and the
// scene has 10 row slots), one fewer than the 11 launcher settings. So the screen shows a
// 10-row window and scrolls it by rebuilding the rows when the cursor moves past the edge.
#include "game.h"

volatile int g_videoActive = 0;

#define VT_AUDIO            0x00A3BA1C
#define ADDR_CALL_BUILD     0x008DEB23
#define ADDR_CALL_HEADER    0x008DEBB7
#define ADDR_HEADER_FN      0x00740E90

typedef enum RowKind { KIND_TOGGLE, KIND_CYCLE, KIND_LIST } RowKind;

typedef struct RowDef
{
    const char* labelKey;
    RowKind kind;
    int id;
} RowDef;

enum
{
    ID_DISPLAY, ID_RESOLUTION, ID_AO, ID_AA, ID_MBLUR, ID_SPLASH, ID_VSYNC,
    ID_WATER, ID_TEXTURE, ID_MODEL, ID_SHADOW
};

// Same order as the launcher.
static const RowDef s_rows[] =
{
    { "MO_ROW_DISPLAY",    KIND_CYCLE,  ID_DISPLAY },
    { "MO_ROW_RESOLUTION", KIND_LIST,   ID_RESOLUTION },
    { "MO_ROW_AO",         KIND_TOGGLE, ID_AO },
    { "MO_ROW_AA",         KIND_TOGGLE, ID_AA },
    { "MO_ROW_MBLUR",      KIND_TOGGLE, ID_MBLUR },
    { "MO_ROW_SPLASH",     KIND_TOGGLE, ID_SPLASH },
    { "MO_ROW_VSYNC",      KIND_TOGGLE, ID_VSYNC },
    { "MO_ROW_WATER",      KIND_CYCLE,  ID_WATER },
    { "MO_ROW_TEXTURE",    KIND_CYCLE,  ID_TEXTURE },
    { "MO_ROW_MODEL",      KIND_CYCLE,  ID_MODEL },
    { "MO_ROW_SHADOW",     KIND_CYCLE,  ID_SHADOW },
};
#define ROW_COUNT ((int)(sizeof(s_rows) / sizeof(s_rows[0])))
#define VISIBLE (ROW_COUNT < MENU_MAX_ROWS ? ROW_COUNT : MENU_MAX_ROWS)

static int s_scroll = 0;        // first logical row shown
static int s_leaving = 0;
static VideoSettings s_edit;    // what the screen shows
static VideoSettings s_saved;   // what's in settings.xml

// MenuBuild header arguments as cOptionsAudio::Enter passed them, reused when scrolling.
static int s_buildA2, s_buildA4, s_buildA5, s_buildScene;
static int s_probed = 0;

typedef void (__thiscall* Method_t)(void* self);
typedef void (__thiscall* Method2_t)(void* self, int a, int b);
static Method_t s_origUpdate, s_origNext, s_origClicked;
static Method2_t s_origValueChanged;

static const RowDef* RowAt(int visibleRow)
{
    int logical = s_scroll + visibleRow;
    return (visibleRow >= 0 && visibleRow < VISIBLE && logical < ROW_COUNT) ? &s_rows[logical] : NULL;
}

// ---- values ----

static int* ToggleField(int id)
{
    switch (id)
    {
    case ID_VSYNC: return &s_edit.vsync;
    case ID_AA: return &s_edit.antialias;
    case ID_AO: return &s_edit.ambientOcclusion;
    case ID_MBLUR: return &s_edit.motionBlur;
    case ID_SPLASH: return &s_edit.screenSplash;
    default: return NULL;
    }
}

static int* CycleField(int id)
{
    switch (id)
    {
    case ID_DISPLAY: return &s_edit.displayMode;
    case ID_WATER: return &s_edit.water;
    case ID_TEXTURE: return &s_edit.texture;
    case ID_MODEL: return &s_edit.model;
    case ID_SHADOW: return &s_edit.shadow;
    default: return NULL;
    }
}

static int CycleValue(int id)
{
    int v = *CycleField(id);
    return v < 0 ? 0 : (v > 2 ? 2 : v);
}

// Screen resolution uses the 3-option selector too: three rotating name slots. Resolution i is
// shown in slot i % 3, so stepping by one always moves to the neighbouring slot like the game's
// own cycling (and animates the same way).
static int ResolutionIndex(void);
static const char* s_resKeys[3] = { "MO_RES_0", "MO_RES_1", "MO_RES_2" };

static void ResolutionName(int index, char* out, size_t size)
{
    const Resolution* res;
    int n = SettingsResolutions(&res);
    if (n > 0 && index >= 0 && index < n)
        _snprintf(out, size, "%d x %d", res[index].width, res[index].height);
    else
        _snprintf(out, size, "%d x %d", s_edit.width, s_edit.height);
    out[size - 1] = 0;
}

static void SetResolutionSlot(int index)
{
    char name[32];
    ResolutionName(index, name, sizeof(name));
    StringsSetText(s_resKeys[((index % 3) + 3) % 3], name);
}

static void SetCycleNames(void* menu, int row, int id)
{
    if (id == ID_RESOLUTION)
    {
        SetResolutionSlot(ResolutionIndex());
        MenuSetCycleNames(menu, row, 3, HashString(s_resKeys[0]), HashString(s_resKeys[1]),
            HashString(s_resKeys[2]));
    }
    else if (id == ID_DISPLAY)
        MenuSetCycleNames(menu, row, 3, HashString("MO_OPT_WINDOWED"), HashString("MO_OPT_FULLSCREEN"),
            HashString("MO_OPT_BORDERLESS"));
    else
        MenuSetCycleNames(menu, row, 3, HashString("MO_OPT_LOW"), HashString("MO_OPT_NORMAL"),
            HashString("MO_OPT_HIGH"));
}

static int ResolutionIndex(void)
{
    const Resolution* res;
    int n = SettingsResolutions(&res);
    int best = 0;
    for (int i = 0; i < n; i++)
    {
        if (res[i].width == s_edit.width && res[i].height == s_edit.height)
            return i;
        if (res[i].width * res[i].height <= s_edit.width * s_edit.height)
            best = i; // custom size: closest smaller-or-equal mode
    }
    return best;
}


// ---- building the rows ----

static void ProbeSlots(void* menu)
{
    // How many row slots does the loaded layout have? (Logged for reference.)
    int slots = 0;
    for (int k = 1; k <= 20; k++)
    {
        char path[96];
        _snprintf(path, sizeof(path), "%s\\TEXT_%d", MENU_PREFIX(menu), k);
        path[sizeof(path) - 1] = 0;
        if (FindTextElement(UI_REGISTRY, HashString(path)))
            slots = k;
    }
    Log(L"Video menu: layout '%hs' has %d row slot(s); menu object holds %d", MENU_PREFIX(menu), slots, MENU_MAX_ROWS);
}

static int s_reveal;    // selectors to show again once the screen has settled
static int s_settled;   // 0 while the screen slides in / out
static int s_holdFrames;   // frames left to hold the selectors in their shown pose


static void RevealSelectors(void* menu, int animate)
{
    for (int r = 0; r < VISIBLE; r++)
    {
        const RowDef* def = RowAt(r);
        if (def->kind == KIND_CYCLE)
            MenuSetCycle(menu, r, CycleValue(def->id), animate);
        else if (def->kind == KIND_LIST)
            MenuSetCycle(menu, r, ResolutionIndex() % 3, animate);
    }
}

// Called by layout.c once per frame from the animation hook (Hook_Update does not run while the
// screen slides in or out): keep the ported selectors in their shown pose during the slides,
// since the compact layout's scene resets them as it comes in.
static void PortFrame(void)
{
    // The compact scene resets the selectors right after it is switched on, so they are put in
    // their shown pose for the first frames only. Their screen position is kept up to date by
    // layout.c (PortTick re-applies their animations every frame), so the pose animation can
    // finish during the slide and doesn't play again when the rows land.
    if (g_videoActive && !s_leaving && s_holdFrames > 0)
    {
        s_holdFrames--;
        RevealSelectors(GAME_MENU(), 0);
    }
}

static int BuildRows(void* menu, int selected)
{
    int a[MENU_MAX_ROWS * 4] = { 0 };
    for (int r = 0; r < VISIBLE; r++)
    {
        const RowDef* def = RowAt(r);
        int type = ROW_BUTTON, value = 0;
        if (def->kind == KIND_TOGGLE)
        {
            type = ROW_TOGGLE;
            value = *ToggleField(def->id) ? 1 : 0;
        }
        else if (def->kind == KIND_CYCLE)
        {
            type = ROW_CYCLE;
            value = CycleValue(def->id);
        }
        else
        {
            type = ROW_CYCLE;
            value = ResolutionIndex() % 3;
        }
        a[r * 4 + 0] = 0;                         // text is a string-table key
        a[r * 4 + 1] = (int)HashString(def->labelKey);
        a[r * 4 + 2] = type;
        a[r * 4 + 3] = value;
    }

    // a3 = layout: 0 normal (Audio Options' own). Its selector rows use the compact layout's
    // selector, ported in by layout.c (PortBegin / PortAfterBuild).
    {
        int types[MENU_MAX_ROWS];
        for (int r = 0; r < VISIBLE; r++)
            types[r] = a[r * 4 + 2];
        PortBegin(types, VISIBLE);
    }
    int result = MenuBuild(menu, s_buildA2, 0, s_buildA4, s_buildA5, s_buildScene, VISIBLE, selected,
        a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
        a[12], a[13], a[14], a[15], a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23],
        a[24], a[25], a[26], a[27], a[28], a[29], a[30], a[31], a[32], a[33], a[34], a[35],
        a[36], a[37], a[38], a[39]);

    {
        int types[MENU_MAX_ROWS];
        for (int r = 0; r < VISIBLE; r++)
            types[r] = a[r * 4 + 2];
        PortAfterBuild(types, VISIBLE);
    }
    // The compact layout's scene (switched on for the ported selectors) puts them back in their
    // hidden state as it comes in, so their reveal is played again once the screen is ready.
    s_reveal = 1;

    for (int r = 0; r < VISIBLE; r++)
    {
        const RowDef* def = RowAt(r);
        if (def->kind == KIND_CYCLE || def->kind == KIND_LIST)
        {
            SetCycleNames(menu, r, def->id);
        }
    }

    if (!s_probed)
    {
        s_probed = 1;
        ProbeSlots(menu);
    }
    return result;
}

// Replaces the audio rows when the video screen is being built.
static int __cdecl VideoMenuBuild(void* menu, int a2, int a3, int a4, int a5, int scene, int count, int selected,
    int r00, int r01, int r02, int r03, int r10, int r11, int r12, int r13, int r20, int r21, int r22, int r23,
    int r30, int r31, int r32, int r33, int r40, int r41, int r42, int r43, int r50, int r51, int r52, int r53,
    int r60, int r61, int r62, int r63)
{
    if (!g_videoActive)
        return MenuBuild(menu, a2, a3, a4, a5, scene, count, selected,
            r00, r01, r02, r03, r10, r11, r12, r13, r20, r21, r22, r23,
            r30, r31, r32, r33, r40, r41, r42, r43, r50, r51, r52, r53, r60, r61, r62, r63);

    s_buildA2 = a2;
    s_buildA4 = a4;
    s_buildA5 = a5;
    s_buildScene = scene;
    s_settled = 0;
    s_holdFrames = 10;
    PortSetFrameCallback(PortFrame);
    if (selected < 0 || selected >= VISIBLE)
        selected = G_AUDIO_ROW = 0;
    return BuildRows(menu, selected);
}

typedef void (__thiscall* Header_t)(void* header, int scene, unsigned int title, unsigned int sub, int a4, int a5);

static void __fastcall VideoHeader(void* header, void* edx, int scene, unsigned int title, unsigned int sub, int a4, int a5)
{
    (void)edx;
    if (g_videoActive)
    {
        title = HashString("MO_TITLE_VIDEO");
        sub = HashString("MO_SUB_VIDEO");
    }
    ((Header_t)ADDR_HEADER_FN)(header, scene, title, sub, a4, a5);
}

// ---- input ----

static void Leave(void* scene, int sound)
{
    s_leaving = 1;
    s_settled = 0;
    PortLeave();
    *((BYTE*)scene + 0x38) = 1;   // "cancelled": stops cOptionsAudio from saving audio values
    PlayUiSound(sound, 0);
    SceneLeave(scene);
}

static void Accept(void* scene)
{
    if (memcmp(&s_edit, &s_saved, sizeof(s_edit)) != 0 && SettingsSave(&s_edit))
    {
        s_saved = s_edit;
        LiveApply(&s_edit);
    }
    Leave(scene, 4);
}

static void Back(void* scene)
{
    s_edit = s_saved;   // discard
    Leave(scene, 9);
}

// Scrolling: the visible rows are re-filled in place instead of rebuilding the menu (a rebuild
// shuts the layer down and starts it again, and the rows vanished until the next selection).
typedef char (__stdcall* ElementCmd_t)(unsigned int hash, int arg);   // ecx unused
#define ElementShowCmd ((ElementCmd_t)0x00493B10)
#define ElementHideCmd ((ElementCmd_t)0x00493B60)
typedef char (__thiscall* SetLocalised_t)(void* owner, unsigned int element, unsigned int text, int arg);
#define SetLocalised ((SetLocalised_t)0x00493DE0)

static unsigned int RowElement(const char* fmt, int row)
{
    char path[96];
    _snprintf(path, sizeof(path), fmt, row + 1);
    path[sizeof(path) - 1] = 0;
    return HashString(path);
}

static unsigned int CycleName(int id, int v)
{
    if (id == ID_RESOLUTION)
        return HashString(s_resKeys[((v % 3) + 3) % 3]);
    if (id == ID_DISPLAY)
        return HashString(v == 0 ? "MO_OPT_WINDOWED" : (v == 1 ? "MO_OPT_FULLSCREEN" : "MO_OPT_BORDERLESS"));
    return HashString(v == 0 ? "MO_OPT_LOW" : (v == 1 ? "MO_OPT_NORMAL" : "MO_OPT_HIGH"));
}

// Shows a selector's value without playing its animation (MenuSetCycle and MenuSetCycleNames
// both play one): option names into the menu's table, the value, and both texts.
static void SetCycleQuiet(void* menu, int row, int id, int v)
{
    unsigned int* names = (unsigned int*)((BYTE*)menu + 0xE4) + row * 4;
    for (int i = 0; i < 3; i++)
        names[i] = CycleName(id, id == ID_RESOLUTION ? (v - v % 3) + i : i);
    if (id == ID_RESOLUTION)
        for (int i = 0; i < 3; i++)
            names[i] = HashString(s_resKeys[i]);
    ((int*)((BYTE*)menu + 0x6C))[row] = id == ID_RESOLUTION ? v % 3 : v;
    int arg = *(int*)((BYTE*)menu + 0x3C);
    unsigned int name = CycleName(id, v);
    SetLocalised(UiTextOwner(), RowElement("VERTICAL_MENU_SMALL\\TOGGLE_TEXT_1_%d", row), name, arg);
    SetLocalised(UiTextOwner(), RowElement("VERTICAL_MENU_SMALL\\TOGGLE_TEXT_2_%d", row), name, arg);
}

static void RefillRows(void* menu)
{
    int types[MENU_MAX_ROWS];
    int shown[MENU_MAX_ROWS] = { 0 };
    for (int r = 0; r < VISIBLE; r++)
    {
        const RowDef* def = RowAt(r);
        int type = def->kind == KIND_TOGGLE ? ROW_TOGGLE : ROW_CYCLE;
        types[r] = type;
        int* typeSlot = (int*)((BYTE*)menu + 0x44) + r;
        int oldType = *typeSlot;
        *typeSlot = type;

        SetLocalised(UiTextOwner(), RowElement("VERTICAL_MENU\\TEXT_%d", r), HashString(def->labelKey), s_buildA2);

        if (type != oldType)
        {
            if (type == ROW_TOGGLE)
                ElementShowCmd(RowElement("VERTICAL_MENU\\TOGGLE_GROUP_%d", r), 1);
            else
                ElementHideCmd(RowElement("VERTICAL_MENU\\TOGGLE_GROUP_%d", r), 1);
            PortShowSelector(r, type == ROW_CYCLE);
        }

        if (type == ROW_TOGGLE)
        {
            // MenuSetToggle skips a value the menu already holds (possibly another row's)
            ((int*)((BYTE*)menu + 0x6C))[r] = *ToggleField(def->id) ? 0 : 1;
            MenuSetToggle(menu, r, *ToggleField(def->id) ? 1 : 0);
        }
        else if (type != oldType)
        {
            // newly shown selector: its pose comes from the game's own call
            shown[r] = 1;
            SetCycleNames(menu, r, def->id);
            MenuSetCycle(menu, r, def->kind == KIND_LIST ? ResolutionIndex() % 3 : CycleValue(def->id), 0);
        }
        else
        {
            if (def->kind == KIND_LIST)
                SetResolutionSlot(ResolutionIndex());
            SetCycleQuiet(menu, r, def->id, def->kind == KIND_LIST ? ResolutionIndex() : CycleValue(def->id));
        }
    }
    PortAfterBuild(types, VISIBLE);   // re-place the selectors for the new row types
    for (int r = 0; r < VISIBLE; r++)
        if (shown[r])
            PortFinishSelector(r);   // skip the pose animation
}

static void MoveCursor(void* menu, int dir)
{
    int row = G_AUDIO_ROW;
    int target = row + dir;

    if (target >= 0 && target < VISIBLE)
    {
        G_AUDIO_ROW = target;
        PlayUiSound(2, 0);
        MenuSelectRow(menu, target, 0, dir > 0, 0, 0, 0);
        return;
    }

    // Past the edge of the window: scroll if there's more.
    int newScroll = s_scroll + dir;
    if (newScroll < 0 || newScroll + VISIBLE > ROW_COUNT)
        return;
    s_scroll = newScroll;
    PlayUiSound(2, 0);
    RefillRows(menu);
}

// dir: -1 left, +1 right; absolute >= 0: value from the mouse.
static void Adjust(void* menu, int row, int dir, int absolute)
{
    const RowDef* def = RowAt(row);
    if (!def)
        return;

    if (def->kind == KIND_TOGGLE)
    {
        int* f = ToggleField(def->id);
        int v = absolute >= 0 ? (absolute != 0) : (dir < 0 ? 1 : 0);   // ON sits on the left
        if (*f == v)
            return;
        *f = v;
        MenuSetToggle(menu, row, v);
    }
    else if (def->kind == KIND_CYCLE)
    {
        int* f = CycleField(def->id);
        int v = absolute >= 0 ? absolute : (CycleValue(def->id) + dir + 3) % 3;   // wraps, like the game's
        if (v < 0 || v > 2 || v == *f)
            return;
        *f = v;
        MenuSetCycle(menu, row, v, 1);
    }
    else
    {
        const Resolution* res;
        int n = SettingsResolutions(&res);
        if (n <= 0)
            return;
        int index = ResolutionIndex();
        if (absolute >= 0)   // mouse on the selector arrows: slot value 0..2 -> step
            dir = ((absolute - index % 3) + 3) % 3 == 1 ? 1 : (((absolute - index % 3) + 3) % 3 == 2 ? -1 : 0);
        int target = index + dir;
        if (target < 0 || target > n - 1 || target == index)
        {
            MenuSetCycle(menu, row, index % 3, 0);   // undo a mouse click past either end
            return;
        }
        if (res[target].width != s_edit.width || res[target].height != s_edit.height)
            s_edit.refresh = res[target].refresh;
        s_edit.width = res[target].width;
        s_edit.height = res[target].height;
        SetResolutionSlot(target);
        MenuSetCycle(menu, row, target % 3, 1);
    }

    PlayUiSound2(2, 0);
    PlayUiSound(2, 0);
}

static int Ready(void* scene)
{
    return *(int*)((BYTE*)scene + 8) == 2 && *(int*)((BYTE*)scene + 0x10) == 0 && !s_leaving;
}

static void __fastcall Hook_Update(void* scene, void* edx)
{
    (void)edx;
    if (!g_videoActive)
    {
        s_origUpdate(scene);
        return;
    }

    if (G_FRONTEND_PAUSED)
        return;
    void* menu = GAME_MENU();
    MenuUpdate(menu);
    if (*(int*)((BYTE*)scene + 8) != 2 || s_leaving)   // still coming in / already leaving
        return;
    s_settled = 1;   // selectors were held visible during the slide; nothing more to do

    unsigned int pressed = GetPressed(G_GAME, G_INPUT_PLAYER);
    if (pressed & IN_ACCEPT)
        Accept(scene);
    else if (pressed & IN_BACK)
        Back(scene);
    else if (pressed & IN_DOWN)
        MoveCursor(menu, +1);
    else if (pressed & IN_UP)
        MoveCursor(menu, -1);
    else if (pressed & IN_LEFT)
        Adjust(menu, G_AUDIO_ROW, -1, -1);
    else if (pressed & IN_RIGHT)
        Adjust(menu, G_AUDIO_ROW, +1, -1);
}

static void __fastcall Hook_ValueChanged(void* scene, void* edx, int row, int value)
{
    (void)edx;
    if (!g_videoActive)
    {
        s_origValueChanged(scene, row, value);
        return;
    }
    if (!Ready(scene))
        return;
    const RowDef* def = RowAt(row);
    if (!def)
        return;
    int maxValue = def->kind == KIND_TOGGLE ? 1 : 2;
    Adjust(GAME_MENU(), row, 0, value < 0 ? 0 : (value > maxValue ? maxValue : value));
}

// A mouse click on a row only changes its value; confirming is done with Accept / the footer.
static void __fastcall Hook_Clicked(void* scene, void* edx)
{
    (void)edx;
    if (!g_videoActive)
        s_origClicked(scene);
}

static void __fastcall Hook_Next(void* scene, void* edx)
{
    (void)edx;
    if (g_videoActive)
    {
        g_videoActive = 0;
        G_AUDIO_ROW = 0;
        PortEnd();
    }
    s_origNext(scene);   // pushes cOptions
}

void VideoMenuOpen(void)
{
    SettingsLoad(&s_saved);
    s_edit = s_saved;
    s_scroll = 0;
    s_leaving = 0;
    G_AUDIO_ROW = 0;
    g_videoActive = 1;
    Log(L"Opening DISPLAY OPTIONS");
}

int VideoMenuInstall(void)
{
    unsigned char callBuild[5] = { 0xE8 };
    *(LONG*)(callBuild + 1) = (LONG)(0x0074BA60 - (ADDR_CALL_BUILD + 5));
    unsigned char callHeader[5] = { 0xE8 };
    *(LONG*)(callHeader + 1) = (LONG)(ADDR_HEADER_FN - (ADDR_CALL_HEADER + 5));

    DWORD* vt = (DWORD*)VT_AUDIO;
    if (!PatchExpect(ADDR_CALL_BUILD, callBuild, 5) || !PatchExpect(ADDR_CALL_HEADER, callHeader, 5) ||
        vt[7] != 0x008DF720 || vt[8] != 0x008DEF50 || vt[10] != 0x008DF1E0 || vt[13] != 0x008DEE80)
    {
        Log(L"Video menu: unexpected code in cOptionsAudio; VIDEO OPTIONS screen disabled");
        return 0;
    }

    s_origValueChanged = (Method2_t)vt[7];
    s_origClicked = (Method_t)vt[8];
    s_origUpdate = (Method_t)vt[10];
    s_origNext = (Method_t)vt[13];

    PatchVtableSlot((DWORD)&vt[7], 0x008DF720, (void*)Hook_ValueChanged);
    PatchVtableSlot((DWORD)&vt[8], 0x008DEF50, (void*)Hook_Clicked);
    PatchVtableSlot((DWORD)&vt[10], 0x008DF1E0, (void*)Hook_Update);
    PatchVtableSlot((DWORD)&vt[13], 0x008DEE80, (void*)Hook_Next);
    PatchRel32(ADDR_CALL_BUILD, 0xE8, (void*)VideoMenuBuild, 5);
    PatchRel32(ADDR_CALL_HEADER, 0xE8, (void*)VideoHeader, 5);

    StringsAdd("MO_TITLE_VIDEO", "DISPLAY OPTIONS");
    StringsAdd("MO_SUB_VIDEO", "Change your display settings");
    StringsAdd("MO_RES_0", "");
    StringsAdd("MO_RES_1", "");
    StringsAdd("MO_RES_2", "");
    StringsAdd("MO_ROW_DISPLAY", "DISPLAY MODE");
    StringsAdd("MO_ROW_RESOLUTION", "SCREEN RESOLUTION");
    StringsAdd("MO_ROW_AO", "AMBIENT OCCLUSION");
    StringsAdd("MO_ROW_AA", "ANTI-ALIASING");
    StringsAdd("MO_ROW_MBLUR", "MOTION BLUR");
    StringsAdd("MO_ROW_SPLASH", "SCREEN SPLASH");
    StringsAdd("MO_ROW_VSYNC", "V-SYNC");
    StringsAdd("MO_ROW_WATER", "WATER QUALITY");
    StringsAdd("MO_ROW_TEXTURE", "TEXTURE QUALITY");
    StringsAdd("MO_ROW_MODEL", "MODEL QUALITY");
    StringsAdd("MO_ROW_SHADOW", "SHADOW QUALITY");
    StringsAdd("MO_OPT_WINDOWED", "WINDOWED");
    StringsAdd("MO_OPT_FULLSCREEN", "FULLSCREEN");
    StringsAdd("MO_OPT_BORDERLESS", "BORDERLESS");
    StringsAdd("MO_OPT_LOW", "LOW");
    StringsAdd("MO_OPT_NORMAL", "NORMAL");
    StringsAdd("MO_OPT_HIGH", "HIGH");

    Log(L"Video menu: installed");
    return 1;
}
