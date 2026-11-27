// DISPLAY OPTIONS in the race pause menu.
//
// The pause menus are lists ("menu definitions", 0x320 bytes, built by 0x723570) set up once by
// the pause object's setup (0x7295D0, called at 0x6766B1). The pause object is the race manager
// [0xE9A408] + 0xE0:
//   +0x038 CONTROL OPTIONS list   +0x678 AUDIO list   +0x998 the OPTIONS list holding both
// A list:
//   +0x050 + i*0x38 rows (max 10): +0x00 callback (member pointer {fn, this adjust, 0, 0},
//          called thiscall(pause object + adjust, int* value) when the value changes)
//          +0x10 label hash  +0x14 type (1 on/off, 3 selector, 4 slider, 5 sub-list)
//          +0x18 shown  +0x1C shown index  +0x20 description hash
//          +0x2C option names (hash array) +0x30 count  +0x34 value (sub-list: the list)
//   +0x2B8 row count  +0x2BC selected row  +0x2C0 shown rows  +0x2DC the menu
//   +0x2E0 pause object (set when shown)
//   +0x2E8 member pointer called thiscall(pause object, list, phase) while it is being shown
//   +0x2F8 member pointer called thiscall(pause object, list, flag) when it is closed
// Shown rows go to menu rows (index - shown + 9): the list fills the bottom, at most 9 rows.
// A list with no sliders uses the compact menu; one slider and it uses the normal one.
// 0x726390 shows a list, 0x727F20 updates it (cursor input through GetPressed at 0x727FC6).
//
// The mod adds a DISPLAY OPTIONS sub-list row to the OPTIONS list. Its list has 9 rows that show
// a window onto the 11 settings and scroll at either end; it uses the normal (Audio Options)
// layout with the selector ported in (layout.c). Changes are saved and applied when it closes.
#include "game.h"

#define ADDR_PAUSE_SETUP       0x007295D0
#define ADDR_CALL_PAUSE_SETUP  0x006766B1
#define ADDR_LIST_CTOR         0x00723570
#define ADDR_LIST_ADD_ROW      0x007238B0
#define ADDR_LIST_ADD_SUBLIST  0x007237D0
#define ADDR_LIST_SET_VALUE    0x00723700
#define ADDR_LIST_SHOW         0x00726390
#define ADDR_LIST_SHOW_DESC    0x00723DB0
#define ADDR_CALL_GETPRESSED   0x00727FC6   // cursor up / down
#define ADDR_CALL_GETPRESSED2  0x00728237   // accept / back / left / right
#define PAUSE_OPTIONS_LIST     0x998
#define LIST_SIZE              0x320
#define LIST_ROWS              9

typedef void (__thiscall* ListCtor_t)(void* list);
typedef void (__thiscall* AddRow_t)(void* list, void* fn, int adj, int z1, int z2,
    unsigned int label, int value, int type, unsigned int desc);
typedef void (__thiscall* AddSubList_t)(void* list, unsigned int label, void* sub, unsigned int desc);
typedef void (__thiscall* SetValue_t)(void* list, unsigned int label, int value);
typedef void (__thiscall* ShowDesc_t)(void* list, int row);
typedef void (__thiscall* ListShow_t)(void* list, void* pause, int a2, int a3, int a4, int a5, int sel);
typedef char (__stdcall* ElementCmd_t)(unsigned int hash, int arg);   // ecx unused
#define ElementShowCmd ((ElementCmd_t)0x00493B10)
#define ElementHideCmd ((ElementCmd_t)0x00493B60)
typedef char (__thiscall* SetLocalised_t)(void* owner, unsigned int element, unsigned int text, int arg);
#define SetLocalised ((SetLocalised_t)0x00493DE0)

enum { P_DISPLAY, P_RESOLUTION, P_AO, P_AA, P_MBLUR, P_SPLASH, P_VSYNC, P_WATER, P_TEXTURE, P_MODEL, P_SHADOW };

typedef struct PauseRow
{
    int id;
    const char* label;
    const char* desc;
    int type;   // 1 on/off, 3 selector
} PauseRow;

static const PauseRow s_settings[] =
{
    { P_DISPLAY,    "MO_ROW_DISPLAY",    "MO_DESC_DISPLAY",    3 },
    { P_RESOLUTION, "MO_ROW_RESOLUTION", "MO_DESC_RESOLUTION", 3 },
    { P_AO,         "MO_ROW_AO",         "MO_DESC_AO",         1 },
    { P_AA,         "MO_ROW_AA",         "MO_DESC_AA",         1 },
    { P_MBLUR,      "MO_ROW_MBLUR",      "MO_DESC_MBLUR",      1 },
    { P_SPLASH,     "MO_ROW_SPLASH",     "MO_DESC_SPLASH",     1 },
    { P_VSYNC,      "MO_ROW_VSYNC",      "MO_DESC_VSYNC",      1 },
    { P_WATER,      "MO_ROW_WATER",      "MO_DESC_WATER",      3 },
    { P_TEXTURE,    "MO_ROW_TEXTURE",    "MO_DESC_TEXTURE",    3 },
    { P_MODEL,      "MO_ROW_MODEL",      "MO_DESC_MODEL",      3 },
    { P_SHADOW,     "MO_ROW_SHADOW",     "MO_DESC_SHADOW",     3 },
};
#define SETTING_COUNT ((int)(sizeof(s_settings) / sizeof(s_settings[0])))

static VideoSettings s_edit, s_saved;
static BYTE* s_list;           // our list (latest)
static int s_scroll;           // first setting shown
static unsigned int s_namesDisplay[3], s_namesQuality[3], s_namesRes[3];
static const char* s_resKeys[3] = { "MO_PRES_0", "MO_PRES_1", "MO_PRES_2" };
static int s_resIndex;   // index into the resolution list
static int s_resSlot;    // name slot (0..2) the selector is showing

static const PauseRow* SettingAt(int row) { return &s_settings[s_scroll + row]; }
static BYTE* ListRow(int row) { return s_list + 0x50 + row * 0x38; }
static int MenuRow(int row) { return row - LIST_ROWS + 9; }

// ---- values ----

static int* Field(int id)
{
    switch (id)
    {
    case P_DISPLAY: return &s_edit.displayMode;
    case P_AO: return &s_edit.ambientOcclusion;
    case P_AA: return &s_edit.antialias;
    case P_MBLUR: return &s_edit.motionBlur;
    case P_SPLASH: return &s_edit.screenSplash;
    case P_VSYNC: return &s_edit.vsync;
    case P_WATER: return &s_edit.water;
    case P_TEXTURE: return &s_edit.texture;
    case P_MODEL: return &s_edit.model;
    case P_SHADOW: return &s_edit.shadow;
    default: return NULL;
    }
}

static int FindResolution(void)
{
    const Resolution* res;
    int n = SettingsResolutions(&res);
    int best = 0;
    for (int i = 0; i < n; i++)
    {
        if (res[i].width == s_edit.width && res[i].height == s_edit.height)
            return i;
        if (res[i].width * res[i].height <= s_edit.width * s_edit.height)
            best = i;
    }
    return best;
}

static void SetResolutionName(int slot, int index)
{
    const Resolution* res;
    int n = SettingsResolutions(&res);
    char name[32];
    if (n > 0 && index >= 0 && index < n)
        _snprintf(name, sizeof(name), "%d x %d", res[index].width, res[index].height);
    else
        _snprintf(name, sizeof(name), "%d x %d", s_edit.width, s_edit.height);
    name[sizeof(name) - 1] = 0;
    StringsSetText(s_resKeys[slot], name);
}

static int SettingValue(int id)
{
    if (id == P_RESOLUTION)
        return s_resSlot;
    int v = *Field(id);
    if (id == P_DISPLAY || id == P_WATER || id == P_TEXTURE || id == P_MODEL || id == P_SHADOW)
        return v < 0 ? 0 : (v > 2 ? 2 : v);
    return v ? 1 : 0;
}

static unsigned int* NamesFor(int id)
{
    return id == P_DISPLAY ? s_namesDisplay : (id == P_RESOLUTION ? s_namesRes : s_namesQuality);
}

// ---- row callbacks: thiscall(pause object, int* value) ----

static void Changed(int id, int value)
{
    if (id == P_RESOLUTION)
    {
        // Three rotating name slots: the selector moves to the next / previous slot, which is
        // given the next / previous resolution's name (or the same one at either end).
        const Resolution* res;
        int n = SettingsResolutions(&res);
        int step = ((value - s_resSlot) + 3) % 3 == 1 ? 1 : -1;
        int target = s_resIndex + step;
        if (n > 0 && target >= 0 && target < n)
        {
            s_resIndex = target;
            if (res[target].width != s_edit.width || res[target].height != s_edit.height)
                s_edit.refresh = res[target].refresh;
            s_edit.width = res[target].width;
            s_edit.height = res[target].height;
        }
        s_resSlot = ((value % 3) + 3) % 3;
        SetResolutionName(s_resSlot, s_resIndex);
        return;
    }
    int* f = Field(id);
    if (f)
        *f = value;
}

#define ROW_CALLBACK(n) \
    static void __fastcall RowChanged##n(void* pause, void* edx, int* value) \
    { (void)pause; (void)edx; Changed(SettingAt(n)->id, *value & 0xFFFF); }
ROW_CALLBACK(0) ROW_CALLBACK(1) ROW_CALLBACK(2) ROW_CALLBACK(3) ROW_CALLBACK(4)
ROW_CALLBACK(5) ROW_CALLBACK(6) ROW_CALLBACK(7) ROW_CALLBACK(8)
static void* const s_callbacks[LIST_ROWS] =
{
    (void*)RowChanged0, (void*)RowChanged1, (void*)RowChanged2, (void*)RowChanged3, (void*)RowChanged4,
    (void*)RowChanged5, (void*)RowChanged6, (void*)RowChanged7, (void*)RowChanged8,
};

// The list's rows describe the settings currently in view.
static void FillListRows(void)
{
    for (int i = 0; i < LIST_ROWS; i++)
    {
        const PauseRow* r = SettingAt(i);
        BYTE* row = ListRow(i);
        *(unsigned int*)(row + 0x10) = HashString(r->label);
        *(int*)(row + 0x14) = r->type;
        *(unsigned int*)(row + 0x20) = HashString(r->desc);
        *(unsigned int**)(row + 0x2C) = r->type == 3 ? NamesFor(r->id) : NULL;
        *(int*)(row + 0x30) = r->type == 3 ? 3 : 0;
        // selectors: low word the value, high word the option count (the list wraps with it)
        *(int*)(row + 0x34) = r->type == 3 ? (3 << 16) | SettingValue(r->id) : SettingValue(r->id);
    }
}

// ---- the selector port ----

static int s_portOn;          // port started for our list
static int s_portPlaced;      // selectors placed (the compact layout can arrive a frame late)
static int s_holdFrames;
static int s_activeOurs;      // our list is the one being shown
static DWORD s_retryStart;
static int s_accepted;        // closed with accept: save and apply

static void SlotTypes(int* types)
{
    for (int r = 0; r < MENU_MAX_ROWS; r++)
        types[r] = 0;
    for (int i = 0; i < LIST_ROWS; i++)
    {
        int slot = MenuRow(i);
        if (slot >= 0 && slot < MENU_MAX_ROWS)
            types[slot] = SettingAt(i)->type;
    }
}

static void PauseEndPort(void)
{
    if (s_portOn)
    {
        s_portOn = 0;
        s_portPlaced = 0;
        PortEnd();
    }
}

static void HoldSelectors(void)
{
    void* menu = *(void**)(s_list + 0x2DC);
    for (int i = 0; i < LIST_ROWS; i++)
    {
        BYTE* row = ListRow(i);
        if (*(int*)(row + 0x14) == 3)
            MenuSetCycle(menu, MenuRow(i), *(int*)(row + 0x34) & 0xFFFF, 0);
    }
}

// Called every frame from layout.c's per-frame point.
static void PauseFrame(void)
{
    if (!s_portOn || !s_list || s_holdFrames <= 0)
        return;
    s_holdFrames--;
    HoldSelectors();
}

// The menu's option names for the selector rows (the list's show leaves whatever the last
// menu with selectors put there, Controller Options' NORMAL / ADVANCED).
static void SetMenuNames(void)
{
    BYTE* menu = *(BYTE**)(s_list + 0x2DC);
    for (int i = 0; i < LIST_ROWS; i++)
    {
        const PauseRow* r = SettingAt(i);
        if (r->type != 3)
            continue;
        unsigned int* names = (unsigned int*)(menu + 0xE4) + MenuRow(i) * 4;
        unsigned int* src = NamesFor(r->id);
        for (int k = 0; k < 3; k++)
            names[k] = src[k];
    }
}

static void TryPlace(void)
{
    int types[MENU_MAX_ROWS];
    SlotTypes(types);
    if (!PortAfterBuild(types, MENU_MAX_ROWS))
    {
        if (GetTickCount() - s_retryStart > 3000)
        {
            Log(L"Pause menu: compact layout never loaded; selectors not placed");
            s_portPlaced = 1;   // stop retrying
        }
        return;
    }
    s_portPlaced = 1;
    for (int r = 0; r < MENU_MAX_ROWS; r++)
        if (types[r] == 3)
            PortShowSelector(r, 1);
    s_holdFrames = 10;
    PortSetFrameCallback(PauseFrame);
}

// ---- list callbacks: thiscall(pause object, list, flag) ----

static void __fastcall ListShown(void* pause, void* edx, void* list, int phase)
{
    (void)pause;
    (void)edx;
    if (phase != 0)
        return;
    // Row 0 was marked as a slider so the list is shown with the normal (Audio Options) layout;
    // it is itself again before the rows are added.
    BYTE* row0 = (BYTE*)list + 0x50;
    if (*(int*)(row0 + 0x14) == 4)
        *(int*)(row0 + 0x14) = SettingAt(0)->type;
    // No SetRowValue here: the shown-row count isn't set yet at this point, so it would write
    // past the menu's value array (into its slider objects). The values are in the rows already.
}

static void __fastcall ListClosed(void* pause, void* edx, void* list, int flag)
{
    (void)pause;
    (void)edx;
    (void)list;
    (void)flag;
    s_activeOurs = 0;
    // The menu slides out now: the selectors follow their buttons until the next list is built.
    if (s_portOn)
        PortLeave();
    int accepted = s_accepted;
    s_accepted = 0;
    // As in the frontend's Display Options: accept saves and applies, back discards.
    if (!accepted)
    {
        s_edit = s_saved;
        return;
    }
    if (memcmp(&s_edit, &s_saved, sizeof(s_edit)) != 0 && SettingsSave(&s_edit))
    {
        s_saved = s_edit;
        LiveApply(&s_edit);
    }
}

// ---- showing a list ----

static BYTE* s_showTrampoline;

static void __fastcall ListShow(BYTE* list, void* edx, void* pause, int a2, int a3, int a4, int a5, int sel)
{
    (void)edx;
    if (!s_list || list != s_list)
    {
        // Another list: end the port after it is built, so the compact layout's elements the
        // previous menu used are not freed while the menu is being rebuilt.
        s_activeOurs = 0;
        ((ListShow_t)(void*)s_showTrampoline)(list, pause, a2, a3, a4, a5, sel);
        int compact = strcmp(MENU_PREFIX(*(void**)(list + 0x2DC)), "VERTICAL_MENU_SMALL") == 0;
        if (s_portOn && compact)
            PortKeepScene();
        PauseEndPort();
        // The pause lists without sliders use the compact layout: same sizing as Controller
        // Options in the frontend.
        if (compact)
            LayoutCompactFix(COMPACT_CONTROLLER);
        return;
    }

    if (!s_activeOurs)
    {
        // Opened from the OPTIONS list: fresh values, view from the top.
        SettingsLoad(&s_edit);
        s_saved = s_edit;
        s_resIndex = FindResolution();
        s_resSlot = 0;
        SetResolutionName(0, s_resIndex);
        s_scroll = 0;
    }
    s_activeOurs = 1;
    FillListRows();

    int types[MENU_MAX_ROWS];
    SlotTypes(types);
    *(int*)(list + 0x50 + 0x14) = 4;   // normal layout (restored in ListShown)
    if (s_portOn)
        PauseEndPort();
    PortBegin(types, MENU_MAX_ROWS);
    s_portOn = 1;
    s_portPlaced = 0;
    s_retryStart = GetTickCount();
    ((ListShow_t)(void*)s_showTrampoline)(list, pause, a2, a3, a4, a5, sel);
    SetMenuNames();
    PortEnsureScene();
    TryPlace();
}

// From layout.c every UI evaluation: the compact layout is registered a little after it is
// switched on in a race, so placing the selectors is retried until it is there.
void PausePortRetry(void)
{
    if (s_portOn && !s_portPlaced && s_list)
        TryPlace();
}

// ---- scrolling ----

static unsigned int RowElement(const char* fmt, int row)
{
    char path[96];
    _snprintf(path, sizeof(path), fmt, row + 1);
    path[sizeof(path) - 1] = 0;
    return HashString(path);
}

// The rows in view changed: update the list's rows and the menu in place.
static void RefillMenu(void)
{
    BYTE* menu = *(BYTE**)(s_list + 0x2DC);
    int arg = *(int*)(menu + 0x3C);
    int shown[MENU_MAX_ROWS] = { 0 };
    FillListRows();
    for (int i = 0; i < LIST_ROWS; i++)
    {
        const PauseRow* r = SettingAt(i);
        int slot = MenuRow(i);
        int* typeSlot = (int*)(menu + 0x44) + slot;
        int oldType = *typeSlot;
        *typeSlot = r->type;
        SetLocalised(UiTextOwner(), RowElement("VERTICAL_MENU\\TEXT_%d", slot), HashString(r->label), arg);
        if (r->type != oldType)
        {
            if (r->type == 1)
                ElementShowCmd(RowElement("VERTICAL_MENU\\TOGGLE_GROUP_%d", slot), 1);
            else
                ElementHideCmd(RowElement("VERTICAL_MENU\\TOGGLE_GROUP_%d", slot), 1);
            PortShowSelector(slot, r->type == 3);
        }
        int v = SettingValue(r->id);
        if (r->type == 1)
        {
            // MenuSetToggle does nothing when the menu already holds that value, and the value
            // left there can be another setting's (or a selector's): always make it apply.
            ((int*)(menu + 0x6C))[slot] = v ? 0 : 1;
            MenuSetToggle(menu, slot, v);
        }
        else
        {
            unsigned int* names = (unsigned int*)(menu + 0xE4) + slot * 4;
            unsigned int* src = NamesFor(r->id);
            for (int k = 0; k < 3; k++)
                names[k] = src[k];
            if (r->type != oldType)
            {
                shown[slot] = 1;
                MenuSetCycle(menu, slot, v, 0);
            }
            else
            {
                ((int*)(menu + 0x6C))[slot] = v;
                SetLocalised(UiTextOwner(), RowElement("VERTICAL_MENU_SMALL\\TOGGLE_TEXT_1_%d", slot), src[v], arg);
                SetLocalised(UiTextOwner(), RowElement("VERTICAL_MENU_SMALL\\TOGGLE_TEXT_2_%d", slot), src[v], arg);
            }
        }
    }
    int types[MENU_MAX_ROWS];
    SlotTypes(types);
    if (PortAfterBuild(types, MENU_MAX_ROWS))
        s_portPlaced = 1;
    for (int r = 0; r < MENU_MAX_ROWS; r++)
        if (shown[r])
            PortFinishSelector(r);
    ((ShowDesc_t)ADDR_LIST_SHOW_DESC)(s_list, *(int*)(s_list + 0x2BC));
}

// Jumps the view and the cursor (wrapping from one end of the settings to the other).
// MenuSelectRow animates the highlight from the current row to the next one up or down, so a
// jump is made of single steps, as if the cursor had been moved row by row.
static void JumpTo(int scroll, int row)
{
    void* menu = *(void**)(s_list + 0x2DC);
    int from = *(int*)(s_list + 0x2BC);
    s_scroll = scroll;
    *(int*)(s_list + 0x2BC) = row;
    PlayUiSound(2, 0);
    RefillMenu();
    int step = row > from ? 1 : -1;
    for (int r = from + step; r != row + step; r += step)
        MenuSelectRow(menu, MenuRow(r), 0, step > 0, 0, 0, 0);
}

// GetPressed for the list being updated: up at the top / down at the bottom scrolls our list,
// and past either end wraps around to the other.
static unsigned int __fastcall ListGetPressed(void* game, void* edx, int pad)
{
    (void)edx;
    unsigned int pressed = GetPressed(game, pad);
    if (!s_activeOurs || !s_list || !s_portOn)
        return pressed;
    int sel = *(int*)(s_list + 0x2BC);
    int maxScroll = SETTING_COUNT - LIST_ROWS;
    if ((pressed & IN_DOWN) && sel == LIST_ROWS - 1)
    {
        if (s_scroll < maxScroll)
        {
            s_scroll++;
            PlayUiSound(2, 0);
            RefillMenu();
        }
        else
            JumpTo(0, 0);
        return pressed & ~(unsigned int)IN_DOWN;
    }
    if ((pressed & IN_UP) && sel == 0)
    {
        if (s_scroll > 0)
        {
            s_scroll--;
            PlayUiSound(2, 0);
            RefillMenu();
        }
        else
            JumpTo(maxScroll, LIST_ROWS - 1);
        return pressed & ~(unsigned int)IN_UP;
    }
    return pressed;
}

// GetPressed for the list's accept / back / left / right: in our list accept closes it (as back
// does, but saving), and on a selector right goes to the next option (the list's own way round
// is left = next).
static unsigned int __fastcall ListGetPressed2(void* game, void* edx, int pad)
{
    (void)edx;
    unsigned int pressed = GetPressed(game, pad);
    if (!s_activeOurs || !s_list || !s_portOn)
        return pressed;
    if (pressed & IN_ACCEPT)
    {
        s_accepted = 1;
        return (pressed & ~(unsigned int)(IN_ACCEPT | IN_LEFT | IN_RIGHT)) | IN_BACK;
    }
    int sel = *(int*)(s_list + 0x2BC);
    if (sel >= 0 && sel < LIST_ROWS && *(int*)(ListRow(sel) + 0x14) == 3)
    {
        unsigned int lr = pressed & (IN_LEFT | IN_RIGHT);
        pressed &= ~(unsigned int)(IN_LEFT | IN_RIGHT);
        if (lr & IN_LEFT) pressed |= IN_RIGHT;
        if (lr & IN_RIGHT) pressed |= IN_LEFT;
    }
    return pressed;
}

// ---- setup ----

static void SetupList(BYTE* pause)
{
    BYTE* options = pause + PAUSE_OPTIONS_LIST;
    if (*(DWORD*)options != 0x00A114B8 || *(int*)(options + 0x2B8) >= 10)
    {
        Log(L"Pause menu: OPTIONS list not found or full");
        return;
    }

    // A new list for every pause object (one per race manager); it stays allocated.
    BYTE* list = (BYTE*)calloc(1, LIST_SIZE + 0x40);
    if (!list)
        return;
    ((ListCtor_t)ADDR_LIST_CTOR)(list);

    s_list = list;
    s_scroll = 0;
    for (int i = 0; i < LIST_ROWS; i++)
    {
        const PauseRow* r = SettingAt(i);
        ((AddRow_t)ADDR_LIST_ADD_ROW)(list, s_callbacks[i], 0, 0, 0, HashString(r->label), 0, r->type,
            HashString(r->desc));
    }
    FillListRows();
    *(void**)(list + 0x2E8) = (void*)ListShown;
    *(int*)(list + 0x2EC) = 0;
    *(void**)(list + 0x2F8) = (void*)ListClosed;
    *(int*)(list + 0x2FC) = 0;

    ((AddSubList_t)ADDR_LIST_ADD_SUBLIST)(options, HashString("MO_PAUSE_DISPLAY"), list, HashString("MO_DESC_PAUSE_DISPLAY"));

    // Above AUDIO, as on the consoles: move the new row (last) up to just after the first one
    // (CONTROLLER OPTIONS), shifting the rows between down.
    int count = *(int*)(options + 0x2B8);
    if (count >= 3)
    {
        BYTE saved[0x38];
        memcpy(saved, options + 0x50 + (count - 1) * 0x38, 0x38);
        memmove(options + 0x50 + 2 * 0x38, options + 0x50 + 1 * 0x38, (count - 2) * 0x38);
        memcpy(options + 0x50 + 1 * 0x38, saved, 0x38);
    }
    Log(L"Pause menu: DISPLAY added above AUDIO (%d rows over %d settings)", LIST_ROWS, SETTING_COUNT);
}

// The race manager (0xE9A408, which holds the pause object at +0xE0) is created before the mod is
// loaded, so its OPTIONS list is extended from the first UI update instead (layout.c calls this
// every UI evaluation). The setup hook below covers a race manager created later.
static BYTE* s_doneFor;

void PauseMenuPoll(void)
{
    PausePortRetry();
    BYTE* game = *(BYTE**)0x00E9A408;
    if (!game || game == s_doneFor)
        return;
    s_doneFor = game;
    BYTE* options = game + 0xE0 + PAUSE_OPTIONS_LIST;
    unsigned int label = HashString("MO_PAUSE_DISPLAY");
    for (int i = 0; i < *(int*)(options + 0x2B8) && i < 10; i++)
        if (*(unsigned int*)(options + 0x50 + i * 0x38 + 0x10) == label)
            return;   // already there
    SetupList(game + 0xE0);
}

typedef void (__thiscall* PauseSetup_t)(void* pause);

static void __fastcall PauseSetup(void* pause, void* edx)
{
    (void)edx;
    ((PauseSetup_t)ADDR_PAUSE_SETUP)(pause);
    SetupList((BYTE*)pause);
}

int PauseMenuInstall(void)
{
    s_namesDisplay[0] = HashString("MO_OPT_WINDOWED");
    s_namesDisplay[1] = HashString("MO_OPT_FULLSCREEN");
    s_namesDisplay[2] = HashString("MO_OPT_BORDERLESS");
    s_namesQuality[0] = HashString("MO_OPT_LOW");
    s_namesQuality[1] = HashString("MO_OPT_NORMAL");
    s_namesQuality[2] = HashString("MO_OPT_HIGH");
    for (int i = 0; i < 3; i++)
    {
        StringsAdd(s_resKeys[i], "");
        s_namesRes[i] = HashString(s_resKeys[i]);
    }
    StringsAdd("MO_PAUSE_DISPLAY", "DISPLAY");
    StringsAdd("MO_DESC_PAUSE_DISPLAY", "Change your display settings");
    StringsAdd("MO_DESC_DISPLAY", "Play in a window, fullscreen or in a borderless window");
    StringsAdd("MO_DESC_RESOLUTION", "Change the screen resolution");
    StringsAdd("MO_DESC_AO", "Turn ambient occlusion on or off");
    StringsAdd("MO_DESC_AA", "Turn anti-aliasing on or off");
    StringsAdd("MO_DESC_MBLUR", "Turn motion blur on or off");
    StringsAdd("MO_DESC_SPLASH", "Turn screen splash effects on or off");
    StringsAdd("MO_DESC_VSYNC", "Turn V-Sync on or off");
    StringsAdd("MO_DESC_WATER", "Change the water quality");
    StringsAdd("MO_DESC_TEXTURE", "Change the texture quality");
    StringsAdd("MO_DESC_MODEL", "Change the model quality");
    StringsAdd("MO_DESC_SHADOW", "Change the shadow quality");

    unsigned char call[5] = { 0xE8 };
    *(LONG*)(call + 1) = (LONG)(ADDR_PAUSE_SETUP - (ADDR_CALL_PAUSE_SETUP + 5));
    if (!PatchExpect(ADDR_CALL_PAUSE_SETUP, call, 5))
    {
        Log(L"Pause menu: unexpected code at 0x%08X; not added", ADDR_CALL_PAUSE_SETUP);
        return 0;
    }
    PatchRel32(ADDR_CALL_PAUSE_SETUP, 0xE8, (void*)PauseSetup, 5);

    // push ebx / push ebp / push esi / mov esi, ecx / xor ebp, ebp
    static const unsigned char showEntry[7] = { 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x33, 0xED };
    if (PatchExpect(ADDR_LIST_SHOW, showEntry, 7))
    {
        s_showTrampoline = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(s_showTrampoline, showEntry, 7);
        s_showTrampoline[7] = 0xE9;
        *(LONG*)(s_showTrampoline + 8) = (LONG)((ADDR_LIST_SHOW + 7) - (DWORD)(s_showTrampoline + 12));
        PatchRel32(ADDR_LIST_SHOW, 0xE9, (void*)ListShow, 7);
    }
    else
        Log(L"Pause menu: unexpected code at 0x%08X; Display Options uses the compact layout", ADDR_LIST_SHOW);

    unsigned char getPressed[5] = { 0xE8 };
    *(LONG*)(getPressed + 1) = (LONG)(0x0068CF30 - (ADDR_CALL_GETPRESSED + 5));
    if (PatchExpect(ADDR_CALL_GETPRESSED, getPressed, 5))
        PatchRel32(ADDR_CALL_GETPRESSED, 0xE8, (void*)ListGetPressed, 5);
    else
        Log(L"Pause menu: unexpected code at 0x%08X; Display Options won't scroll", ADDR_CALL_GETPRESSED);

    *(LONG*)(getPressed + 1) = (LONG)(0x0068CF30 - (ADDR_CALL_GETPRESSED2 + 5));
    if (PatchExpect(ADDR_CALL_GETPRESSED2, getPressed, 5))
        PatchRel32(ADDR_CALL_GETPRESSED2, 0xE8, (void*)ListGetPressed2, 5);
    else
        Log(L"Pause menu: unexpected code at 0x%08X; accept won't save Display", ADDR_CALL_GETPRESSED2);

    Log(L"Pause menu: installed");
    return 1;
}
