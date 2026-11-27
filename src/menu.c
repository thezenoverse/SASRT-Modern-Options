// Adds a "DISPLAY OPTIONS" row to the game's Options menu (cOptions), in the slot the Xbox 360
// version has it: CONTROLLER, AUDIO, DISPLAY, CREDITS, SET DEFAULT.
//
// How the stock menu works (ASN_App_PcDx9_Final.exe, Steam build, image base 0x400000):
//   cOptions::Enter   0x8C61D0  builds the list with MenuBuild(0x74BA60): 4 rows
//                               {literal, text, type, value} = CONTROL, AUDIO, CREDITS, SET DEFAULT
//   cOptions::Update  0x8C6470  input: down stops at row 3, A on row 3 = reset defaults,
//                               A on any other row = leave the screen
//   cOptions::Exit    0x8C6770  switch(row): 0 Controller, 1 Audio, 2 Credits
//   g_optionsRow      0xC567E0  selected row, also restored when coming back to the menu
//
// With the row inserted at index 2:
//   - MenuBuild gets 5 rows; ours is literal text (printed with "%s"), no string-table entry
//   - the "down" limit and the reset-defaults row become 4
//   - A on row 2 opens the mod's DISPLAY OPTIONS screen (video.c), which runs inside the
//     cOptionsAudio scene class
//   - Exit maps rows 0,1,2,3 to the stock cases 0,1,1,2; row 2 lands on Audio, which video.c
//     takes over while g_videoActive is set
#include "game.h"

#define ADDR_MENUBUILD          0x0074BA60
#define ADDR_CALL_MENUBUILD     0x008C62CE
#define ADDR_SELECT_CHECK       0x008C64E9
#define ADDR_SELECT_TO_EXIT     0x008C64F2
#define ADDR_SELECT_TO_RESET    0x008C64FE
#define ADDR_UPDATE_DONE        0x008C661F
#define ADDR_DOWN_LIMIT_IMM     0x008C6540
#define ADDR_EXIT_READ_ROW      0x008C67EC
#define ADDR_OPTIONS_ROW        0x00C567E0

#define DISPLAY_ROW 2
static const char s_displayLabel[] = "DISPLAY OPTIONS";


// Replaces the 24-argument MenuBuild call in cOptions::Enter:
//   (menu, a2, a3, a4, a5, scene, count, selected, rows[count] {type, text, p1, p2})
static int __cdecl OptionsMenuBuild(int menu, int a2, int a3, int a4, int a5, int scene, int count, int selected,
    int r0t, int r0h, int r0a, int r0b,
    int r1t, int r1h, int r1a, int r1b,
    int r2t, int r2h, int r2a, int r2b,
    int r3t, int r3h, int r3a, int r3b)
{
    (void)count;
    int result = MenuBuild((void*)menu, a2, a3, a4, a5, scene, 5, selected,
        r0t, r0h, r0a, r0b,
        r1t, r1h, r1a, r1b,
        1, (int)s_displayLabel, 0, 0,
        r2t, r2h, r2a, r2b,
        r3t, r3h, r3a, r3b);
    LayoutFixApply();   // VERTICAL_MENU's elements exist once a menu has been built with it
    return result;
}

void __cdecl OnVideoOptionsSelected(void)
{
    VideoMenuOpen();
}

// A pressed on the Options menu. Replaces "cmp [row], 3 / je reset" (9 bytes).
// Reached with a jmp; esi = cOptions this, which every target expects untouched.
void MO_SelectStub(void);
__asm__(
    ".text\n"
    ".globl _MO_SelectStub\n"
    "_MO_SelectStub:\n"
    "    movl 0x00C567E0, %eax\n"
    "    cmpl $2, %eax\n"
    "    je 1f\n"
    "    cmpl $4, %eax\n"
    "    je 2f\n"
    "    pushl $0x008C64F2\n"   // leave the screen -> cOptions::Exit opens the submenu
    "    ret\n"
    "2:  pushl $0x008C64FE\n"   // SET DEFAULT OPTIONS confirmation
    "    ret\n"
    "1:  pushal\n"
    "    call _OnVideoOptionsSelected\n"
    "    popal\n"
    "    pushl $0x008C64F2\n"   // leave the screen -> Exit opens the display screen
    "    ret\n");

// Replaces "mov eax, [row]" at the top of cOptions::Exit's switch with a call that returns
// the stock case for the row: 0 -> 0, 1 -> 1, 2 (DISPLAY) -> 1 (Audio host), 3 -> 2.
void MO_ExitRowStub(void);
__asm__(
    ".text\n"
    ".globl _MO_ExitRowStub\n"
    "_MO_ExitRowStub:\n"
    "    movl 0x00C567E0, %eax\n"
    "    cmpl $2, %eax\n"
    "    jb 1f\n"
    "    decl %eax\n"
    "1:  ret\n");

int PatchExpect(DWORD addr, const void* bytes, size_t len)
{
    return memcmp((const void*)addr, bytes, len) == 0;
}

void PatchWrite(DWORD addr, const void* bytes, size_t len)
{
    DWORD old;
    VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, bytes, len);
    VirtualProtect((void*)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
}

void PatchRel32(DWORD addr, unsigned char opcode, const void* target, size_t padTo)
{
    unsigned char buf[16];
    buf[0] = opcode;
    *(LONG*)(buf + 1) = (LONG)((DWORD)target - (addr + 5));
    for (size_t i = 5; i < padTo; i++)
        buf[i] = 0x90;
    PatchWrite(addr, buf, padTo < 5 ? 5 : padTo);
}

int PatchVtableSlot(DWORD slotAddr, DWORD expected, void* hook)
{
    if (*(DWORD*)slotAddr != expected)
        return 0;
    DWORD value = (DWORD)hook;
    PatchWrite(slotAddr, &value, 4);
    return 1;
}

int MenuPatchInstall(void)
{
    unsigned char callBuild[5] = { 0xE8 };
    *(LONG*)(callBuild + 1) = (LONG)(ADDR_MENUBUILD - (ADDR_CALL_MENUBUILD + 5));
    static const unsigned char selectCheck[9] = { 0x83, 0x3D, 0xE0, 0x67, 0xC5, 0x00, 0x03, 0x74, 0x0C };
    static const unsigned char downLimit[3] = { 0x83, 0xF8, 0x03 };
    static const unsigned char exitRead[5] = { 0xA1, 0xE0, 0x67, 0xC5, 0x00 };

    // Only patch the exact build this was written against.
    if (!PatchExpect(ADDR_CALL_MENUBUILD, callBuild, 5) ||
        !PatchExpect(ADDR_SELECT_CHECK, selectCheck, 9) ||
        !PatchExpect(ADDR_DOWN_LIMIT_IMM - 2, downLimit, 3) ||
        !PatchExpect(ADDR_EXIT_READ_ROW, exitRead, 5))
    {
        Log(L"Options menu: unexpected code at the patch sites (different game version?); DISPLAY OPTIONS not added");
        return 0;
    }

    PatchRel32(ADDR_CALL_MENUBUILD, 0xE8, (void*)OptionsMenuBuild, 5);
    PatchRel32(ADDR_SELECT_CHECK, 0xE9, (void*)MO_SelectStub, 9);
    unsigned char four = 4;
    PatchWrite(ADDR_DOWN_LIMIT_IMM, &four, 1);
    PatchRel32(ADDR_EXIT_READ_ROW, 0xE8, (void*)MO_ExitRowStub, 5);

    Log(L"Options menu: DISPLAY OPTIONS row added");
    return 1;
}
