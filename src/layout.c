// Menu layout fixes.
//
// The vertical menu has two layouts: VERTICAL_MENU (normal rows) and VERTICAL_MENU_SMALL. Only
// the small one draws the named options of a multi-choice row (ROW_CYCLE: "TOGGLE_3_OPTION",
// "TOGGLE_TEXT_1/2", "TOGGLE_POINTER"), because the PC port points the normal layout's entries
// at "VERTICAL_MENU\DUMMY_%d" in this table:
//
//   0xB7C908  normal: POS_POINTER, LEFT_POINTER, RIGHT_POINTER, SLIDER_BACK,
//                     DUMMY,       DUMMY,          DUMMY,         DUMMY
//   0xB7C928  small:  POS_POINTER, LEFT_POINTER, RIGHT_POINTER, SLIDER_BACK,
//                     TOGGLE_3_OPTION, TOGGLE_POINTER, TOGGLE_TEXT_1, TOGGLE_TEXT_2
//
// So Controller Options (which has ON/OFF and A/B rows) was moved to the small layout, whose
// rows are taller on PC, instead of the normal-sized rows the Xbox 360 version shows.
//
// When the loaded menu scene still has the normal layout's toggle text elements, this restores
// the table and builds Controller Options (and DISPLAY OPTIONS) with the normal layout again.
// Controller Options also gets a menu position (the MENUPOS_GROUP "POS_n" animation the menu
// plays when shown, which can move and resize it). Position 0 matches the Xbox 360 version;
// debug builds can step it with F6/F7.
#include "game.h"

#define TABLE_NORMAL            0x00B7C908
#define ADDR_CALL_CTRL_BUILD    0x008DD479

typedef void* (__thiscall* FindTexture_t)(void* registry, unsigned int hash);
#define FindTextureElement ((FindTexture_t)0x004972A0)

volatile int g_normalToggles = 0;
// The PC port draws the compact layout bigger than the Xbox 360 version: its rows and text are
// scaled by this, and the extra space it adds between rows is removed.
static float s_compactScale = 0.77f;   // (1 while a wide menu is being set up)
static const int s_compactSpacing = 1;
volatile int g_controllerMenuPos = 0;
static int s_checked = 0;

static const char s_fmtToggle3[] = "VERTICAL_MENU\\TOGGLE_3_OPTION_%d";
static const char s_fmtPointer[] = "VERTICAL_MENU\\TOGGLE_POINTER_%d";
static const char s_fmtText1[] = "VERTICAL_MENU\\TOGGLE_TEXT_1_%d";
static const char s_fmtText2[] = "VERTICAL_MENU\\TOGGLE_TEXT_2_%d";

static int HasText(const char* path)
{
    return FindTextElement(UI_REGISTRY, HashString(path)) != NULL;
}

static int HasTexture(const char* path)
{
    return FindTextureElement(UI_REGISTRY, HashString(path)) != NULL;
}

void LayoutFixApply(void)
{
    if (s_checked)
        return;
    // The registry only knows a layout's elements after a menu was built with it.
    if (!HasText("VERTICAL_MENU\\TEXT_1"))
    {
        Log(L"Layout: VERTICAL_MENU not loaded yet; checking again later");
        return;
    }
    s_checked = 1;

    int text1 = HasText("VERTICAL_MENU\\TOGGLE_TEXT_1_1");
    int text2 = HasText("VERTICAL_MENU\\TOGGLE_TEXT_2_1");
    int pointer = HasTexture("VERTICAL_MENU\\TOGGLE_POINTER_1") || HasText("VERTICAL_MENU\\TOGGLE_POINTER_1");
    Log(L"Layout: normal menu toggle text 1=%d 2=%d pointer=%d | small: text1=%d pointer=%d | normal TEXT_1=%d",
        text1, text2, pointer, HasText("VERTICAL_MENU_SMALL\\TOGGLE_TEXT_1_1"),
        HasTexture("VERTICAL_MENU_SMALL\\TOGGLE_POINTER_1") || HasText("VERTICAL_MENU_SMALL\\TOGGLE_POINTER_1"),
        HasText("VERTICAL_MENU\\TEXT_1"));

    if (!text1 || !text2)
    {
        Log(L"Layout: the normal menu has no toggle text elements; Controller Options keeps the PC layout");
        return;
    }

    DWORD* table = (DWORD*)TABLE_NORMAL;
    DWORD entries[4] = { (DWORD)s_fmtToggle3, pointer ? (DWORD)s_fmtPointer : table[5], (DWORD)s_fmtText1, (DWORD)s_fmtText2 };
    PatchWrite((DWORD)&table[4], entries, sizeof(entries));
    g_normalToggles = 1;
    Log(L"Layout: restored the normal menu's toggle elements%s", pointer ? L"" : L" (no mouse pointer for them)");
}

// ---------------------------------------------------------------------------------------------
// Compact layout sizing.
//
// Element memory (registry node embedded at +0x04):
//   +0x08/+0x0C tree links, +0x14 name hash, +0x3C/+0x40 local position, +0x44/+0x48 local
//   scale, +0xE0 -> definition entry {hash, four-cc type, ...}, +0xE8 parent element.
// On PC every compact row group "VERTICAL_MENU_SMALL\n" sits inside an extra unnamed group whose
// local y grows by 0.0139 per row (about 10 px at 720p); the normal layout has those at 0.
// The compact rows are also drawn bigger. This pulls the spacers back to 0 and scales each row
// (and, so text stays in proportion, the text inside it) by CompactScale.

static BYTE* FindElement(unsigned int hash)
{
    BYTE* node = *(BYTE**)UI_REGISTRY;
    int guard = 0;
    while (node && guard++ < 64)
    {
        unsigned int key = *(unsigned int*)(node + 0x10);
        if (hash > key)
            node = *(BYTE**)(node + 8);
        else if (hash < key)
            node = *(BYTE**)(node + 4);
        else
            return node - 4;
    }
    return NULL;
}

static int IsType(const BYTE* el, const char* fourcc)
{
    const BYTE* def = *(const BYTE* const*)(el + 0xE0);
    return def && memcmp(def + 4, fourcc, 4) == 0;
}


static const BYTE* s_rows[MENU_MAX_ROWS];

// ---------------------------------------------------------------------------------------------
// The UI re-evaluates every element's animation track each frame (0x499440, called from
// 0x495806 with edi = element + 0x30) and writes the result over local position (+0x3C/+0x40)
// and scale (+0x44/+0x48), so direct writes don't stick. The call is wrapped instead and the
// size change is applied to the freshly evaluated values for the compact menu's elements.

#define ADDR_CALL_ANIM_EVAL 0x00495806
#define ADDR_ANIM_EVAL      0x00499440
#define MAX_TARGETS         1024
#define TARGET_SLOTS        2048   // open-addressing table, power of two

enum { T_SHRINK = 1, T_SPACER = 2, T_KEEP = 3, T_HILITE = 4, T_OPTGROUP = 5, T_LEAF = 6, T_SHIFTX = 7, T_SCALEX = 8, T_HIDE = 9, T_PORT = 10 };

static void TraceEval(BYTE* el);
static void TraceFrame(void);
static int s_evalCount;

// LayoutCompactFix modes.
static int s_mode;   // COMPACT_CONTROLLER or COMPACT_WIDE

// Normal-layout look for the compact menu (Display Options): the PC's normal buttons end at
// 1580 px of 1920, the compact ones at 960 px.
#define WIDE_SHIFT   (620.0f / 1080.0f)
#define WIDE_POINTER (27.5714f / 15.0f)   // POS_POINTER hit area width, normal / compact
#define HILITE_NUDGE (2.0f / 1080.0f)     // measured: compact highlight sat 2 px low

// Selector ported into a normal row: its box is widened to cover the normal ON/OFF pair. Measured
// at 1080p: the pair spans 975..1533 (558 px); the compact box texture is 8 px margin + 196 px
// visible, scaled from its left edge. The right arrow follows the box's right edge, the texts
// its centre, and the mouse area grows with it.
#define SEL_BOX_SCALE   (558.0f / 196.0f)
// Horizontal positions are in units of the screen width (1920 px), vertical ones of the height.
#define SEL_ARROW_SHIFT ((204.0f * (SEL_BOX_SCALE - 1.0f) - 14.0f) / 1920.0f)   // -14: measured gap
#define SEL_TEXT_SHIFT  (106.0f * (SEL_BOX_SCALE - 1.0f) / 1920.0f)
#define SEL_POINTER     2.5f
#define PORT_OFFSET_X 0.0669f    // wide box over the ON/OFF pair (see SEL_*)
#define PORT_OFFSET_Y 0.0028f

// Measured from 1080p captures of the PC compact layout (units of screen height):
#define BUTTON_H     (66.0f / 1080.0f)   // button texture height
#define HILITE_H     (80.0f / 1080.0f)   // highlight texture height
#define HILITE_OVER  ( 7.0f / 1080.0f)   // highlight overhang above/below the button (compact)
#define HILITE_OVER_NORMAL (10.5f / 1080.0f) // overhang in the normal menus (Options, Audio)
#define OPTION_H     (39.0f / 1080.0f)   // ON/OFF box height

// Compact geometry captured when the menu is built (local units, 1 = screen height).
static float s_row1Y;     // row 1 local y
static float s_pitch;     // row-to-row pitch without the PC spacer
static float s_step;      // PC spacer per row
static float s_btnOff;    // button texture's y inside its row
typedef struct { BYTE* el; unsigned int hash; int kind; float spacer; BYTE* a; BYTE* b; BYTE* c; float last; int haveLast; } Target;
static Target s_targets[MAX_TARGETS];
static volatile int s_targetCount;
static BYTE* s_targetMin;
static BYTE* s_targetMax;

static unsigned short s_slots[TARGET_SLOTS];   // index + 1 into s_targets, 0 = empty

static unsigned int SlotOf(const BYTE* el)
{
    return ((unsigned int)(ULONG_PTR)el >> 4) * 2654435761u >> 21 & (TARGET_SLOTS - 1);
}

static void ClearTargets(void)
{
    s_targetCount = 0;
    s_targetMin = s_targetMax = NULL;
    memset(s_slots, 0, sizeof(s_slots));
}

static void AddTarget(BYTE* el, int kind, float spacer)
{
    if (!el || s_targetCount >= MAX_TARGETS)
        return;
    unsigned int i = SlotOf(el);
    while (s_slots[i])
    {
        if (s_targets[s_slots[i] - 1].el == el)
            return;   // already registered
        i = (i + 1) & (TARGET_SLOTS - 1);
    }
    Target* t = &s_targets[s_targetCount];
    t->el = el;
    t->hash = *(unsigned int*)(el + 0x14);
    t->kind = kind;
    t->spacer = spacer;
    t->a = t->b = t->c = NULL;
    t->haveLast = 0;
    s_slots[i] = (unsigned short)(++s_targetCount);
    if (!s_targetMin || el < s_targetMin) s_targetMin = el;
    if (el > s_targetMax) s_targetMax = el;
}

static Target* LookupTarget(const BYTE* el)
{
    unsigned int i = SlotOf(el);
    while (s_slots[i])
    {
        Target* t = &s_targets[s_slots[i] - 1];
        if (t->el == el)
            return t;
        i = (i + 1) & (TARGET_SLOTS - 1);
    }
    return NULL;
}

// Selector group of the compact layout placed in a normal row: a = normal TOGGLE_GROUP_n (its
// place in the row), b = the compact row it hangs from. pos = the group's local x, y.
static void PortPlace(const Target* t, float* pos)
{
    // World matrices (+0x10C x, +0x118 y) include the menu's slide in / out; the local
    // positions don't (the slide is applied above the rows).
    // The row's place is taken from its button texture (c): world minus its own offset.
    BYTE* tg = t->a;
    BYTE* ref = t->c ? t->c : *(BYTE**)(tg + 0xE8);
    float rx = *(float*)(ref + 0x10C) - (t->c ? *(float*)(ref + 0x3C) : 0.0f);
    float ry = *(float*)(ref + 0x118) - (t->c ? *(float*)(ref + 0x40) : 0.0f);
    float tx = rx + *(float*)(tg + 0x3C) + PORT_OFFSET_X;
    float ty = ry + *(float*)(tg + 0x40) + PORT_OFFSET_Y;
    pos[0] = tx - *(float*)(t->b + 0x10C);
    pos[1] = ty - *(float*)(t->b + 0x118);
}

static BYTE* s_portClock;            // a normal-layout element evaluated every frame
static void PortReapply(void);
static int s_portLeaving;            // the screen is sliding out
static BYTE* s_exitClock;            // element used as the per-frame point while leaving
static BYTE* RootOf(BYTE* el);
static int IsPortPart(const BYTE* el);
static int s_exitLog;
static BYTE* s_portParts[160];
static int s_portPartCount;
static BYTE* s_portRoot;
static void (*s_portFrame)(void);
void PortSetFrameCallback(void (*fn)(void)) { s_portFrame = fn; }
void PortTick(void);

void __cdecl AnimEvalPost(BYTE* out)
{
    BYTE* el = out - 0x30;
    TraceEval(el);
    PauseMenuPoll();
    s_evalCount++;
    if (s_portClock)
    {
        // Placement: once per frame, at the first evaluation more than 4 ms after the previous
        // frame's (only writes positions, so it is safe anywhere in the UI update).
        static LARGE_INTEGER last, freq;
        LARGE_INTEGER now;
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        int newFrame = (now.QuadPart - last.QuadPart) * 1000 > freq.QuadPart * 4;
        last = now;
        if (newFrame)
        {
            PortTick();
            TraceFrame();
        }

        // Keeping the selectors shown calls back into the menu, so it only runs at a fixed
        // point of the update: when row 1's button is evaluated (as during the slide in).
        // While leaving, row 1's button is no longer evaluated through here; use the first
        // element of another layer that is, so the selectors keep following their rows.
        // While leaving, row 1's button is no longer evaluated through here (and no other
        // element is, for more than a frame or two), so the selector parts are re-applied at
        // the first evaluation of each frame instead.
        if (s_portLeaving && newFrame)
            PortReapply();
        static int busy;
        if (el == s_portClock && !busy)
        {
            busy = 1;
            if (s_portFrame)
                s_portFrame();
            PortReapply();
            busy = 0;
        }
    }
    if (el < s_targetMin || el > s_targetMax)
        return;
    Target* t = LookupTarget(el);
    if (!t)
        return;
    {
        if (*(unsigned int*)(el + 0x14) != t->hash)
            return;   // element memory reused by something else
        switch (t->kind)
        {
        case T_SHRINK: *(float*)(out + 0x18) *= s_compactScale; break;   // scale y
        case T_SPACER:   // position y; t->last is the layout's own value (only adjusted once)
            if (*(float*)(out + 0x10) > t->last - t->spacer * 0.5f)
                *(float*)(out + 0x10) -= t->spacer;
            break;
        case T_KEEP:   *(float*)(out + 0x18) /= s_compactScale; break;   // undo the row's scale y
        case T_SHIFTX:
        case T_SCALEX:
        {
            // A track that doesn't animate this value leaves the previous result in place
            // (0x499440 skips it), and targets are registered again on every scroll, so only a
            // value that still looks unadjusted (compared with the layout's own value in
            // t->last) gets the change; it never adds up.
            float* v = (float*)(out + (t->kind == T_SHIFTX ? 0x0C : 0x14));
            if (t->kind == T_SHIFTX)
            {
                if (*v < t->last + t->spacer * 0.5f)
                    *v += t->spacer;
            }
            else if (*v < t->last * (1.0f + t->spacer) * 0.5f)
                *v *= t->spacer;
            break;
        }
        case T_HIDE:
            *(float*)(out + 0x14) = 0.0f;
            *(float*)(out + 0x18) = 0.0f;
            break;
        case T_PORT:
            PortPlace(t, (float*)(out + 0x0C));
            break;
        case T_HILITE:
        {
            if (s_mode == COMPACT_WIDE)
            {
                // Normal highlight texture on a normal-looking button: only the PC spacer.
                float* y = (float*)(out + 0x10);
                float pitch = s_pitch + s_step;
                float n = pitch > 0.0f ? (*y - s_btnOff - s_row1Y) / pitch : 0.0f;
                if (s_compactSpacing)
                    *y -= n * s_step;
                break;
            }
            // The highlight is placed on the selected button's old top edge; move it to the new
            // one (rows lose the PC spacer and buttons shrink toward their row's origin).
            float* y = (float*)(out + 0x10);
            float pitch = s_pitch + s_step;
            float n = pitch > 0.0f ? (*y - s_btnOff - s_row1Y) / pitch : 0.0f;
            if (s_compactSpacing)
                *y -= n * s_step;
            *y -= (1.0f - s_compactScale) * s_btnOff;
            // Keep the same overhang around the smaller button instead of scaling it.
            // Match the normal menus: same overhang around the button as Options/Audio.
            float k = (BUTTON_H * s_compactScale + 2.0f * HILITE_OVER_NORMAL) / HILITE_H;
            *y += HILITE_OVER * k - HILITE_OVER_NORMAL - HILITE_NUDGE;
            *(float*)(out + 0x18) *= k;
            break;
        }
        case T_LEAF:
        {
            // Inside an option group (scaled with the row): keep full size and keep the offset
            // from the group's boxes, so ON/OFF text stays centred in its box.
            float* y = (float*)(out + 0x10);
            *y = t->spacer + (*y - t->spacer) / s_compactScale;
            *(float*)(out + 0x18) /= s_compactScale;
            break;
        }
        case T_OPTGROUP:
            // Options keep full size inside a smaller button: move them up by half the height
            // difference so they stay centred (the group's position is in the row's scaled space).
            *(float*)(out + 0x10) -= (1.0f - s_compactScale) * OPTION_H * 0.5f / s_compactScale;
            break;
        }
        return;
    }
}

DWORD s_animRet __attribute__((used));

// eax = track, edi = output (element + 0x30), 4 stack args, callee pops them (ret 0x10).
__attribute__((naked)) static void AnimEvalStub(void)
{
    __asm__ volatile(
        "popl _s_animRet\n\t"
        "movl $0x00499440, %%ecx\n\t"
        "call *%%ecx\n\t"
        "pushal\n\t"
        "pushl %%edi\n\t"
        "call _AnimEvalPost\n\t"
        "addl $4, %%esp\n\t"
        "popal\n\t"
        "jmp *_s_animRet\n\t"
        ::: "memory");
}

// The row's scale reaches everything inside it. The label text and the leaves of the
// option/slider groups get their scale divided back so they keep their size; the groups
// themselves keep the row's scale so their contents still follow the smaller button.
static int InRowGroup(const BYTE* el)
{
    const BYTE* p = *(const BYTE* const*)(el + 0xE8);
    for (int i = 0; i < 6 && p; i++)
    {
        const BYTE* pp = *(const BYTE* const*)(p + 0xE8);
        for (int r = 0; r < MENU_MAX_ROWS; r++)
            if (s_rows[r] && pp == s_rows[r])
                return IsType(p, "GROP");
        p = pp;
    }
    return 0;
}

// Top of the boxes in an option group: smallest local y among its texture children.
static void FindRefY(BYTE* node, int depth, const BYTE* parent, float* best)
{
    if (!node || depth > 64)
        return;
    FindRefY(*(BYTE**)(node + 4), depth + 1, parent, best);
    BYTE* el = node - 4;
    if (*(BYTE**)(el + 0xE8) == parent && IsType(el, "TXTR"))
    {
        float y = *(float*)(el + 0x40);
        if (y < *best)
            *best = y;
    }
    FindRefY(*(BYTE**)(node + 8), depth + 1, parent, best);
}

static float GroupRefY(const BYTE* parent)
{
    static const BYTE* cachedParent;
    static float cachedY;
    if (parent == cachedParent)
        return cachedY;
    float best = 1e9f;
    FindRefY(*(BYTE**)UI_REGISTRY, 0, parent, &best);
    if (best > 1e8f)
        best = 0.0f;
    cachedParent = parent;
    cachedY = best;
    return best;
}

// ---- texture definition swaps (compact element draws the normal layout's texture) ----
typedef struct { BYTE* el; unsigned int hash; void* original; } Swap;
static Swap s_swaps[64];
static int s_swapCount;

static void SwapDefinition(BYTE* el, const BYTE* from)
{
    if (!el || !from || s_swapCount >= 64)
        return;
    Swap* sw = &s_swaps[s_swapCount++];
    sw->el = el;
    sw->hash = *(unsigned int*)(el + 0x14);
    sw->original = *(void**)(el + 0xE0);
    *(void**)(el + 0xE0) = *(void* const*)(from + 0xE0);
}

void LayoutRestoreDefinitions(void)
{
    for (int i = s_swapCount - 1; i >= 0; i--)
    {
        Swap* sw = &s_swaps[i];
        if (*(unsigned int*)(sw->el + 0x14) == sw->hash)
            *(void**)(sw->el + 0xE0) = sw->original;
    }
    s_swapCount = 0;
}

typedef struct { BYTE* parent; BYTE* list[8]; int n; } ChildScan;

static void ScanTextures(BYTE* node, int depth, ChildScan* cs)
{
    if (!node || depth > 64)
        return;
    ScanTextures(*(BYTE**)(node + 4), depth + 1, cs);
    BYTE* el = node - 4;
    if (*(BYTE**)(el + 0xE8) == cs->parent && IsType(el, "TXTR") && cs->n < 8)
        cs->list[cs->n++] = el;
    ScanTextures(*(BYTE**)(node + 8), depth + 1, cs);
}

// Texture children of a row in sibling order (+0x134 = next sibling).
static int RowTextures(BYTE* row, BYTE** out)
{
    ChildScan cs;
    memset(&cs, 0, sizeof(cs));
    cs.parent = row;
    ScanTextures(*(BYTE**)UI_REGISTRY, 0, &cs);
    // order: an element no sibling points to comes first
    int n = 0;
    BYTE* cur = NULL;
    for (int i = 0; i < cs.n && !cur; i++)
    {
        int pointed = 0;
        for (int j = 0; j < cs.n; j++)
            if (*(BYTE**)(cs.list[j] + 0x134) == cs.list[i])
                pointed = 1;
        if (!pointed)
            cur = cs.list[i];
    }
    while (cur && n < cs.n)
    {
        out[n++] = cur;
        BYTE* next = NULL;
        // follow the sibling chain, skipping non-texture siblings
        for (BYTE* s = *(BYTE**)(cur + 0x134); s && !next; s = *(BYTE**)(s + 0x134))
            for (int j = 0; j < cs.n; j++)
                if (cs.list[j] == s)
                    next = s;
        cur = next;
    }
    return n;
}

static void CollectRowContent(BYTE* node, int depth, int* count)
{
    if (!node || depth > 64)
        return;
    CollectRowContent(*(BYTE**)(node + 4), depth + 1, count);
    BYTE* el = node - 4;
    BYTE* parent = *(BYTE**)(el + 0xE8);
    if (parent)
    {
        int isRowChild = 0;
        for (int r = 0; r < MENU_MAX_ROWS; r++)
            if (s_rows[r] && parent == s_rows[r])
                isRowChild = 1;
        if (isRowChild)
        {
            if (s_mode == COMPACT_WIDE)
            {
                if (IsType(el, "GROP"))
                    AddTarget(el, T_SHIFTX, WIDE_SHIFT);
                else if (IsType(el, "PNTR"))
                    AddTarget(el, T_SCALEX, WIDE_POINTER);
                else if (IsType(el, "TXTR") && parent == s_rows[0] && s_btnOff == 0.0f)
                    s_btnOff = *(float*)(el + 0x40);
            }
            else if (IsType(el, "TEXT"))
            {
                AddTarget(el, T_KEEP, 0);
                (*count)++;
            }
            else if (IsType(el, "GROP"))
                AddTarget(el, T_OPTGROUP, 0);
            else if (IsType(el, "TXTR") && parent == s_rows[0] && s_btnOff == 0.0f)
                s_btnOff = *(float*)(el + 0x40);
        }
        else if (s_mode != COMPACT_WIDE && !IsType(el, "GROP") && InRowGroup(el))
        {
            AddTarget(el, T_LEAF, GroupRefY(parent));
            (*count)++;
        }
    }
    CollectRowContent(*(BYTE**)(node + 8), depth + 1, count);
}

void LayoutCompactFix(int mode)
{
    LayoutRestoreDefinitions();
    BYTE* menuGroup = FindElement(HashString("VERTICAL_MENU_SMALL\\MENUGROUP"));
    if (!menuGroup)
        return;

    ClearTargets();
    s_mode = mode;
    float savedScale = s_compactScale;
    if (mode == COMPACT_WIDE)
        s_compactScale = 1.0f;   // normal textures already have the normal size

    BYTE* wrappers[MENU_MAX_ROWS] = { 0 };
    int rows = 0, spacers = 0, texts = 0;
    for (int r = 0; r < MENU_MAX_ROWS; r++)
    {
        char name[64];
        _snprintf(name, sizeof(name), "VERTICAL_MENU_SMALL\\%d", r + 1);
        name[sizeof(name) - 1] = 0;
        BYTE* row = FindElement(HashString(name));
        s_rows[r] = row;
        if (!row)
            continue;
        rows++;
        BYTE* w = *(BYTE**)(row + 0xE8);
        if (w && w != menuGroup && *(BYTE**)(w + 0xE8) == menuGroup)
            wrappers[r] = w;
        if (s_compactScale != 1.0f)
            AddTarget(row, T_SHRINK, 0);
    }

    // PC spacer step between compact rows (10/720 of the screen per row).
    float step = 0.0f;
    if (wrappers[0] && wrappers[1])
        step = *(float*)(wrappers[1] + 0x40) - *(float*)(wrappers[0] + 0x40);
    // The adjusted positions can still be in place when this runs again (the pause lists), so
    // the layout's own spacing is kept from the first time it was seen.
    static float s_layoutStep, s_layoutY0;
    if (step > 0.0f && step < 0.05f && wrappers[0])
    {
        s_layoutStep = step;
        s_layoutY0 = *(float*)(wrappers[0] + 0x40);
    }
    else
        step = s_layoutStep;
    if (s_compactSpacing && step > 0.0f && step < 0.05f)
    {
        for (int r = 1; r < MENU_MAX_ROWS; r++)
        {
            if (!wrappers[r])
                continue;
            AddTarget(wrappers[r], T_SPACER, step * (float)r);
            Target* t = LookupTarget(wrappers[r]);
            if (t)
                t->last = s_layoutY0 + step * (float)r;
            spacers++;
        }
    }

    s_step = step;
    s_row1Y = s_rows[0] ? *(float*)(s_rows[0] + 0x40) : 0.0f;
    s_pitch = (s_rows[0] && s_rows[1]) ? *(float*)(s_rows[1] + 0x40) - s_row1Y : 0.0625f;
    s_btnOff = 0.0f;

    BYTE* highlight = NULL;
    int swapped = 0;
    if (s_compactScale != 1.0f || s_compactSpacing || mode == COMPACT_WIDE)
    {
        CollectRowContent(*(BYTE**)UI_REGISTRY, 0, &texts);
        highlight = FindElement(HashString("VERTICAL_MENU_SMALL\\HI_LIGHT"));
        AddTarget(highlight, T_HILITE, 0);
    }
    if (mode == COMPACT_WIDE)
    {
        // Draw the compact buttons and highlight with the normal layout's textures.
        for (int r = 0; r < MENU_MAX_ROWS; r++)
        {
            char name[64];
            _snprintf(name, sizeof(name), "VERTICAL_MENU\\%d", r + 1);
            name[sizeof(name) - 1] = 0;
            BYTE* normalRow = FindElement(HashString(name));
            if (!normalRow || !s_rows[r])
                continue;
            BYTE* a[8];
            BYTE* b[8];
            int na = RowTextures((BYTE*)s_rows[r], a);
            int nb = RowTextures(normalRow, b);
            for (int i = 0; i < na && i < nb; i++, swapped++)
                SwapDefinition(a[i], b[i]);
        }
        BYTE* normalHighlight = FindElement(HashString("VERTICAL_MENU\\HI_LIGHT"));
        if (highlight && normalHighlight)
        {
            SwapDefinition(highlight, normalHighlight);
            swapped++;
        }
    }
    s_compactScale = savedScale;
    if (mode == COMPACT_WIDE)
        Log(L"Compact layout (wide): %d texture definition(s) swapped", swapped);

    Log(L"Compact layout: %d row(s), %d spacer(s) (step %.4f), %d label/option element(s) kept, highlight %s, row1 %.4f pitch %.4f button %.4f, scale %.2f",
        rows, spacers, (double)step, texts, highlight ? L"found" : L"missing",
        (double)s_row1Y, (double)s_pitch, (double)s_btnOff, (double)s_compactScale);
}

// Replaces the MenuBuild call in cOptionsController::Enter (40 arguments: header + 8 rows).
static int __cdecl ControllerMenuBuild(void* menu, int a2, int small, int a4, int pos, int scene, int count, int selected,
    int r00, int r01, int r02, int r03, int r10, int r11, int r12, int r13, int r20, int r21, int r22, int r23,
    int r30, int r31, int r32, int r33, int r40, int r41, int r42, int r43, int r50, int r51, int r52, int r53,
    int r60, int r61, int r62, int r63, int r70, int r71, int r72, int r73)
{
    LayoutFixApply();
    if (g_normalToggles)
        small = 0;
    // The menu position (MENUPOS_GROUP "POS_n" animation, played when the menu is shown) moves
    // and can resize the whole menu; 0 is what the PC port uses here.
    pos = g_controllerMenuPos;
    Log(L"Controller Options: %s layout, menu position %d", small ? L"compact" : L"normal", pos);
    int result = MenuBuild(menu, a2, small, a4, pos, scene, count, selected,
        r00, r01, r02, r03, r10, r11, r12, r13, r20, r21, r22, r23,
        r30, r31, r32, r33, r40, r41, r42, r43, r50, r51, r52, r53,
        r60, r61, r62, r63, r70, r71, r72, r73);
    if (small)
        LayoutCompactFix(COMPACT_CONTROLLER);
    return result;
}

int LayoutFixInstall(void)
{
    unsigned char call[5] = { 0xE8 };
    *(LONG*)(call + 1) = (LONG)(0x0074BA60 - (ADDR_CALL_CTRL_BUILD + 5));
    if (!PatchExpect(ADDR_CALL_CTRL_BUILD, call, 5))
    {
        Log(L"Layout: unexpected code in cOptionsController; layout fix disabled");
        return 0;
    }
    PatchRel32(ADDR_CALL_CTRL_BUILD, 0xE8, (void*)ControllerMenuBuild, 5);

    unsigned char evalCall[5] = { 0xE8 };
    *(LONG*)(evalCall + 1) = (LONG)(ADDR_ANIM_EVAL - (ADDR_CALL_ANIM_EVAL + 5));
    if (PatchExpect(ADDR_CALL_ANIM_EVAL, evalCall, 5))
        PatchRel32(ADDR_CALL_ANIM_EVAL, 0xE8, (void*)AnimEvalStub, 5);
    else
        Log(L"Layout: unexpected code at the animation evaluate call; compact sizing disabled");
    Log(L"Layout: installed (Controller Options menu position %d, compact scale %.2f, spacing fix %d)",
        g_controllerMenuPos, (double)s_compactScale, s_compactSpacing);
    return 1;
}

void LayoutStepControllerPos(int dir)
{
    int v = g_controllerMenuPos + dir;
    if (v < 0) v = 0;
    if (v > 63) v = 63;
    g_controllerMenuPos = v;
    Log(L"F6/F7: Controller Options menu position = %d (reopen Controller Options to see it)", v);
}

// =============================================================================================
// Selector port: the 3-option selector ("TOGGLE_3_OPTION") only exists in VERTICAL_MENU_SMALL.
// A menu built with the normal layout (Display Options) borrows it:
//   - the normal layout's element-name table slots 4..7 (stock: VERTICAL_MENU\DUMMY_%d) are
//     pointed at the compact layout's TOGGLE_3_OPTION / TOGGLE_POINTER / TOGGLE_TEXT_1/2 names,
//     so the menu code drives the compact selector elements for ROW_CYCLE rows;
//   - the compact layout's scene is activated alongside the normal one, everything in it except
//     the selector groups is hidden (scale 0 each frame), and each used selector group is moved
//     every frame onto the matching normal row.
// =============================================================================================

#define TABLE_SMALL            (TABLE_NORMAL + 0x20)
#define ADDR_SCENE_ACTIVE      0x00493F50   // bool __thiscall (owner, hash)
#define ADDR_SCENE_ACTIVATE    0x00493780   // bool __thiscall (owner, hash, 1, arg)
#define ADDR_SCENE_DEACTIVATE  0x004937F0   // __thiscall (owner, hash)

typedef char (__thiscall* SceneActive_t)(void* owner, unsigned int hash);
typedef char (__thiscall* SceneActivate_t)(void* owner, unsigned int hash, int one, int arg);
typedef void (__thiscall* SceneDeactivate_t)(void* owner, unsigned int hash);
typedef void (__thiscall* ElementVis_t)(void* el, int arg);

static DWORD s_savedNormal[4];
static int s_portActive;
static int s_portActivatedScene;

static BYTE* RootOf(BYTE* el)
{
    if (!el)
        return NULL;
    for (int i = 0; i < 32 && el; i++)
    {
        BYTE* p = *(BYTE**)(el + 0xE8);
        if (!p)
            return el;
        el = p;
    }
    return el;
}

static void ElementShow(BYTE* el, int show)
{
    if (!el)
        return;
    void** vt = *(void***)el;
    ((ElementVis_t)vt[(show ? 0x4C : 0x54) / 4])(el, 1);
}

static BYTE* FindSmallSelector(int r)
{
    char name[64];
    _snprintf(name, sizeof(name), "VERTICAL_MENU_SMALL\\TOGGLE_3_OPTION_%d", r + 1);
    name[sizeof(name) - 1] = 0;
    return FindElement(HashString(name));
}

// A selector that was just shown gets its pose from MenuSetCycle(..., 0), which plays the
// TOGGLE_END segment: frames 53..72 on the group, ..68 on its parts (measured). Jump straight to
// the end so nothing visibly plays.
void PortFinishSelector(int row)
{
    BYTE* g = FindSmallSelector(row);
    if (!g)
        return;
    *(int*)(g + 0x34) = 72;
    *(float*)(g + 0x78) = 72.0f;
    for (int i = 0; i < s_portPartCount; i++)
    {
        BYTE* el = s_portParts[i];
        if (el == g || *(int*)(el + 0x34) == 0)
            continue;
        for (BYTE* p = *(BYTE**)(el + 0xE8); p; p = *(BYTE**)(p + 0xE8))
        {
            if (p == g)
            {
                *(int*)(el + 0x34) = 68;
                *(float*)(el + 0x78) = 68.0f;
                break;
            }
        }
    }
}

void PortShowSelector(int row, int show)
{
    ElementShow(FindSmallSelector(row), show);
}

void PortBegin(const int* types, int count)
{
    if (!s_portActive)
    {
        DWORD* table = (DWORD*)TABLE_NORMAL;
        DWORD* small = (DWORD*)TABLE_SMALL;
        memcpy(s_savedNormal, &table[4], sizeof(s_savedNormal));
        PatchWrite((DWORD)&table[4], &small[4], 4 * sizeof(DWORD));
        s_portActive = 1;
    }
    void* owner = UiTextOwner();
    unsigned int scene = HashString("VERTICAL_MENU_SMALL");
    if (!((SceneActive_t)ADDR_SCENE_ACTIVE)(owner, scene))
    {
        ((SceneActivate_t)ADDR_SCENE_ACTIVATE)(owner, scene, 1, 0);
        s_portActivatedScene = 1;
    }
    // Show the selector groups before the menu is built: MenuBuild then plays their reveal
    // animation itself (showing a group afterwards resets it to hidden).
    for (int r = 0; r < MENU_MAX_ROWS; r++)
        ElementShow(FindSmallSelector(r), r < count && types[r] == ROW_CYCLE);
}

static BYTE* s_portGroups[MENU_MAX_ROWS];

// The menu switching from the compact layout to the normal one shuts the compact scene down
// (0x74B270, from MenuBuild), as in the pause menu coming from the OPTIONS list: it is switched
// on again after the build.
void PortEnsureScene(void)
{
    void* owner = UiTextOwner();
    unsigned int scene = HashString("VERTICAL_MENU_SMALL");
    if (!((SceneActive_t)ADDR_SCENE_ACTIVE)(owner, scene))
    {
        ((SceneActivate_t)ADDR_SCENE_ACTIVATE)(owner, scene, 1, 0);
        s_portActivatedScene = 1;
    }
}

// The next menu uses the compact scene itself: PortEnd leaves it on.
void PortKeepScene(void)
{
    s_portActivatedScene = 0;
}


static int IsPortPart(const BYTE* el)
{
    for (int i = 0; i < s_portPartCount; i++)
        if (s_portParts[i] == el)
            return 1;
    return 0;
}

static int UnderPortGroup(BYTE* el)
{
    for (int i = 0; i < 32 && el; i++)
    {
        for (int r = 0; r < MENU_MAX_ROWS; r++)
            if (s_portGroups[r] && el == s_portGroups[r])
                return 1;
        el = *(BYTE**)(el + 0xE8);
    }
    return 0;
}

static void HideCompactScene(BYTE* node, int depth, int* hidden)
{
    if (!node || depth > 64)
        return;
    HideCompactScene(*(BYTE**)(node + 4), depth + 1, hidden);
    BYTE* el = node - 4;
    if (RootOf(el) == s_portRoot)
    {
        if (UnderPortGroup(el))
        {
            if (s_portPartCount < 160)
                s_portParts[s_portPartCount++] = el;
        }
        else if (!IsType(el, "GROP"))
        {
            AddTarget(el, T_HIDE, 0);
            (*hidden)++;
        }
    }
    HideCompactScene(*(BYTE**)(node + 8), depth + 1, hidden);
}

// types: the row types the menu was built with.
int PortAfterBuild(const int* types, int count)
{
    s_portLeaving = 0;
    s_exitClock = NULL;
    LayoutRestoreDefinitions();
    ClearTargets();
    s_mode = COMPACT_WIDE;
    memset(s_portGroups, 0, sizeof(s_portGroups));

    BYTE* menuGroup = FindElement(HashString("VERTICAL_MENU_SMALL\\MENUGROUP"));
    if (!menuGroup)
    {
        return 0;
    }
    s_portRoot = RootOf(menuGroup);

    int ported = 0;
    for (int r = 0; r < MENU_MAX_ROWS; r++)
    {
        char name[64];
        BYTE* group = FindSmallSelector(r);
        _snprintf(name, sizeof(name), "VERTICAL_MENU\\TOGGLE_GROUP_%d", r + 1);
        name[sizeof(name) - 1] = 0;
        BYTE* toggleGroup = FindElement(HashString(name));
        if (!group)
            continue;
        if (r < count && types[r] == ROW_CYCLE && toggleGroup && *(BYTE**)(toggleGroup + 0xE8) && *(BYTE**)(group + 0xE8))
        {
            s_portGroups[r] = group;
            AddTarget(group, T_PORT, 0);
            Target* t = LookupTarget(group);
            if (t)
            {
                BYTE* tex[8];
                t->a = toggleGroup;
                t->b = *(BYTE**)(group + 0xE8);
                t->c = RowTextures(*(BYTE**)(toggleGroup + 0xE8), tex) > 0 ? tex[0] : NULL;
            }
            ported++;
        }
    }

    {
        // Per-frame point: the button of the first row in use (pause lists fill the bottom rows).
        int first = 0;
        while (first < count - 1 && types[first] == 0)
            first++;
        char name[64];
        _snprintf(name, sizeof(name), "VERTICAL_MENU\\%d", first + 1);
        name[sizeof(name) - 1] = 0;
        BYTE* normalRow = FindElement(HashString(name));
        BYTE* tex[8];
        s_portClock = (normalRow && RowTextures(normalRow, tex) > 0) ? tex[0] : NULL;
    }
    int hidden = 0;
    s_portPartCount = 0;
    HideCompactScene(*(BYTE**)UI_REGISTRY, 0, &hidden);

    // Widen the ported selectors (normal style only).
    for (int i = 0; i < s_portPartCount; i++)
    {
        BYTE* el = s_portParts[i];
        float sx = *(float*)(el + 0x44);
        float x = *(float*)(el + 0x3C);
        // reference = the compact layout's own value (from the layout dumps)
        float ref = 0.0f;
        int kind = 0;
        float amount = 0.0f;
        // Classified by position (the box's states sit at x 0.0172, the arrows at 0.0078 and
        // 0.1211): a state texture that is hidden at the moment can have scale 0.
        if (IsType(el, "TXTR") && (sx > 0.5f || (x > 0.012f && x < 0.03f)))
            kind = T_SCALEX, amount = SEL_BOX_SCALE, ref = 0.6009f;      // box
        else if (IsType(el, "TXTR") && x > 0.05f)
            kind = T_SHIFTX, amount = SEL_ARROW_SHIFT, ref = 0.1211f;    // right arrow
        else if (IsType(el, "TEXT"))
            kind = T_SHIFTX, amount = SEL_TEXT_SHIFT, ref = 0.0672f;     // option names, centred
        else if (IsType(el, "PNTR"))
            kind = T_SCALEX, amount = SEL_POINTER, ref = 4.5714f;        // mouse area
        if (kind)
        {
            AddTarget(el, kind, amount);
            Target* t = LookupTarget(el);
            if (t)
                t->last = ref;
        }
    }
    Log(L"Selector port: %d selector(s) placed in normal rows (%d parts), %d compact element(s) hidden%s",
        ported, s_portPartCount, hidden, s_portActivatedScene ? L" (compact scene activated)" : L"");
    return 1;
}

// Every frame: the group's own animation only runs while it changes value, but the rows it is
// placed against keep moving (menu intro), so its position is refreshed here too.
// Research: log how row 1's pieces move while the screen opens / closes.
static int s_traceFrames;
static BYTE* s_traceEl[3];
static int s_traceBase[3];
static DWORD s_traceSnap[3][0x80];
static int s_traceInit[3];
static DWORD s_traceStart;
static void TraceSetup(void);
void PortTrace(int frames) { s_traceFrames = frames; TraceSetup(); }

static void TraceDiff(int k, const char* name)
{
    BYTE* el = s_traceEl[k];
    if (!el)
        return;
    DWORD* cur = (DWORD*)(el + s_traceBase[k]);
    char line[1200];
    int len = _snprintf(line, sizeof(line), "%s t=%lu:", name, GetTickCount() - s_traceStart);
    int changes = 0;
    for (int i = 0; i < 0x80 && len < 1100; i++)
    {
        if (!s_traceInit[k] || cur[i] != s_traceSnap[k][i])
        {
            if (s_traceInit[k])
                len += _snprintf(line + len, sizeof(line) - len, " +%03X=%08lX(%.3f)", s_traceBase[k] + i * 4, cur[i], (double)*(float*)&cur[i]);
            changes++;
            s_traceSnap[k][i] = cur[i];
        }
    }
    line[sizeof(line) - 1] = 0;
    if (!s_traceInit[k])
        Log(L"TRACE %S snapshot taken", line);
    else if (changes)
        Log(L"TRACE %S", line);
    s_traceInit[k] = 1;
}

static void TraceEval(BYTE* el) { (void)el; }

static void TraceFrame(void)
{
    if (s_traceFrames <= 0)
        return;
    s_traceFrames--;
    {
        // selector groups: animation frame; their box state textures: scale / flags
        char line[1500];
        int len = _snprintf(line, sizeof(line), "SCROLLTRACE");
        for (int r = 0; r < MENU_MAX_ROWS && len < 1300; r++)
        {
            BYTE* g = s_portGroups[r];
            if (!g)
                continue;
            len += _snprintf(line + len, sizeof(line) - len, " | r%d f%d/%.1f", r, *(int*)(g + 0x34), (double)*(float*)(g + 0x78));
            for (int i = 0; i < s_portPartCount && len < 1300; i++)
            {
                BYTE* el = s_portParts[i];
                if (*(BYTE**)(el + 0xE8) == g && IsType(el, "TXTR"))
                    len += _snprintf(line + len, sizeof(line) - len, " %.2f/%.2f/%d", (double)*(float*)(el + 0x3C),
                        (double)*(float*)(el + 0x44), *(int*)(el + 0x34));
            }
        }
        line[sizeof(line) - 1] = 0;
        Log(L"%S", line);
        return;
    }
    BYTE* g = NULL;
    for (int r = 0; r < MENU_MAX_ROWS && !g; r++)
        g = s_portGroups[r];
    Log(L"EXIT evals=%d button world %.3f,%.3f d4=%08X | selector local %.3f world %.3f",
        s_evalCount, s_portClock ? (double)*(float*)(s_portClock + 0x10C) : 0.0,
        s_portClock ? (double)*(float*)(s_portClock + 0x118) : 0.0, s_portClock ? *(unsigned int*)(s_portClock + 0xD4) : 0,
        g ? (double)*(float*)(g + 0x3C) : 0.0, g ? (double)*(float*)(g + 0x10C) : 0.0);
    s_evalCount = 0;
}

static BYTE* FindSceneNode(unsigned int hash)
{
    BYTE* node = *(BYTE**)UiTextOwner();
    for (int i = 0; node && i < 64; i++)
    {
        unsigned int key = *(unsigned int*)(node + 0x20);
        if (hash > key) node = *(BYTE**)(node + 8);
        else if (hash < key) node = *(BYTE**)(node + 4);
        else return node;
    }
    return NULL;
}

static void TraceSetup(void)
{
    s_traceStart = GetTickCount();
    return;
    // Layer objects (+0xF8 of every element; the world matrix code multiplies by matrices at
    // +0x38 / +0x64 / +0x90 of it).
    BYTE* normalRow = FindElement(HashString("VERTICAL_MENU\\1"));
    BYTE* smallRow = FindElement(HashString("VERTICAL_MENU_SMALL\\1"));
    s_traceEl[0] = normalRow ? *(BYTE**)(normalRow + 0xF8) : NULL;
    s_traceEl[1] = smallRow ? *(BYTE**)(smallRow + 0xF8) : NULL;
    s_traceEl[2] = (BYTE*)GAME_MENU();
    s_traceBase[0] = s_traceBase[1] = s_traceBase[2] = 0;
    memset(s_traceInit, 0, sizeof(s_traceInit));
    s_traceStart = GetTickCount();
    Log(L"TRACE setup: layer %p small layer %p menu %p", s_traceEl[0], s_traceEl[1], s_traceEl[2]);
}

void PortTick(void)
{

    for (int r = 0; r < MENU_MAX_ROWS; r++)
    {
        BYTE* g = s_portGroups[r];
        if (!g)
            continue;
        Target* t = LookupTarget(g);
        if (t && t->kind == T_PORT && *(unsigned int*)(g + 0x14) == t->hash)
            PortPlace(t, (float*)(g + 0x3C));
    }
}

// An element's screen position is only refreshed when its animation is applied (vtable slot
// 0xB0, 0x4957E0 for every element type: evaluate the current track, then mark it for update).
// The selector parts' animations stop once they reach their pose, so they are applied every
// frame to keep them on their rows. Called at the same fixed point as the selector reveal.
static void PortReapply(void)
{
    typedef void (__thiscall* Apply_t)(void* el);
    static int busy;
    if (busy)
        return;
    busy = 1;
    for (int i = 0; i < s_portPartCount; i++)
    {
        BYTE* el = s_portParts[i];
        ((Apply_t)((*(void***)el)[0xB0 / 4]))(el);
    }
    busy = 0;
}

// The screen is leaving (slide out): keep the selectors following their rows from another
// per-frame point (see AnimEvalPost).
void PortLeave(void)
{
    s_portLeaving = 1;
    s_exitClock = NULL;
    s_exitLog = 8;
}

void PortEnd(void)
{
    s_portPartCount = 0;
    s_portLeaving = 0;
    s_exitClock = NULL;
    if (s_portActive)
    {
        PatchWrite(TABLE_NORMAL + 4 * 4, s_savedNormal, sizeof(s_savedNormal));
        s_portActive = 0;
    }
    for (int r = 0; r < MENU_MAX_ROWS; r++)
        if (s_portGroups[r])
            ElementShow(s_portGroups[r], 0);
    memset(s_portGroups, 0, sizeof(s_portGroups));
    ClearTargets();
    s_portClock = NULL;
    s_portFrame = NULL;
    if (s_portActivatedScene)
    {
        ((SceneDeactivate_t)ADDR_SCENE_DEACTIVATE)(UiTextOwner(), HashString("VERTICAL_MENU_SMALL"));
        s_portActivatedScene = 0;
    }
    Log(L"Selector port: ended");
}
