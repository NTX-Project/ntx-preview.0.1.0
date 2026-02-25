#include "../inc/hal.h"
#include "halp_aur64.h"

enum {
    HALP_COM1_PORT = 0x3F8,
    HALP_PS2_DATA_PORT = 0x60,
    HALP_PS2_STATUS_PORT = 0x64
};

enum {
    HALP_CONSOLE_CELL_W = 9u,
    HALP_CONSOLE_CELL_H = 16u
};

static const BOOT_INFO *gHalBootInfo;
static HAL_PLATFORM_INFO gHalPlatformInfo;
static HAL_TIMER_TICK_ROUTINE gHalTimerTickRoutine;
static HAL_PAGE_FAULT_ROUTINE gHalPageFaultRoutine;
static uint32_t *gHalFb;
static uint32_t gHalFbWidth;
static uint32_t gHalFbHeight;
static uint32_t gHalFbPitch;
static uint32_t gHalFbColor;
static uint32_t gHalFbBg;
static uint32_t gHalFbCursorX;
static uint32_t gHalFbCursorY;
static uint32_t gHalFbCols;
static uint32_t gHalFbRows;
static uint32_t gHalFbReady;
static uint8_t gHalFont8x16[128][16];
static uint32_t gHalFontReady;
static uint8_t gHalPs2ShiftLeft;
static uint8_t gHalPs2ShiftRight;
static uint8_t gHalPs2CapsLock;
static uint8_t gHalPs2Extended;

static const char gHalPs2Map[128] = {
    0, 0, '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7',
    '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static const char gHalPs2MapShift[128] = {
    0, 0, '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7',
    '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static void HalpInitializeSerial(void) {
    HalpIoWrite8(HALP_COM1_PORT + 1, 0x00);
    HalpIoWrite8(HALP_COM1_PORT + 3, 0x80);
    HalpIoWrite8(HALP_COM1_PORT + 0, 0x03);
    HalpIoWrite8(HALP_COM1_PORT + 1, 0x00);
    HalpIoWrite8(HALP_COM1_PORT + 3, 0x03);
    HalpIoWrite8(HALP_COM1_PORT + 2, 0xC7);
    HalpIoWrite8(HALP_COM1_PORT + 4, 0x0B);
}

static void HalpHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t i;
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Out[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void HalpWriteHexLine(const char *Prefix, uint64_t Value) {
    char Buffer[96];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    HalpHex64(Value, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

static void HalpSerialWriteChar(char Ch) {
    uint32_t Spins = 0u;
    while ((HalpIoRead8(HALP_COM1_PORT + 5) & 0x20u) == 0u) {
        Spins++;
        if (Spins >= 1000000u) {
            return;
        }
    }
    HalpIoWrite8(HALP_COM1_PORT, (uint8_t)Ch);
}

static uint32_t HalpSerialTryReadChar(char *OutChar) {
    if (OutChar == 0) {
        return 0u;
    }

    if ((HalpIoRead8(HALP_COM1_PORT + 5) & 0x01u) == 0u) {
        return 0u;
    }

    *OutChar = (char)HalpIoRead8(HALP_COM1_PORT);
    return 1u;
}

static uint32_t HalpPs2TryReadChar(char *OutChar) {
    uint8_t Status;
    uint8_t ScanCode;
    uint8_t IsBreak;
    uint8_t Code;
    uint8_t Shifted;
    char Ch;
    uint32_t IsLetter;

    if (OutChar == 0) {
        return 0u;
    }

    Status = HalpIoRead8(HALP_PS2_STATUS_PORT);
    if ((Status & 0x01u) == 0u) {
        return 0u;
    }

    ScanCode = HalpIoRead8(HALP_PS2_DATA_PORT);
    if (ScanCode == 0xE0u) {
        gHalPs2Extended = 1u;
        return 0u;
    }
    if (ScanCode == 0xE1u) {
        gHalPs2Extended = 0u;
        return 0u;
    }

    IsBreak = (uint8_t)((ScanCode & 0x80u) != 0u);
    Code = (uint8_t)(ScanCode & 0x7Fu);

    if (Code == 0x2Au) {
        gHalPs2ShiftLeft = (uint8_t)(IsBreak == 0u);
        gHalPs2Extended = 0u;
        return 0u;
    }
    if (Code == 0x36u) {
        gHalPs2ShiftRight = (uint8_t)(IsBreak == 0u);
        gHalPs2Extended = 0u;
        return 0u;
    }
    if (Code == 0x3Au && IsBreak == 0u) {
        gHalPs2CapsLock ^= 1u;
        gHalPs2Extended = 0u;
        return 0u;
    }

    if (IsBreak != 0u) {
        gHalPs2Extended = 0u;
        return 0u;
    }

    if (gHalPs2Extended != 0u) {
        gHalPs2Extended = 0u;
        if (Code == 0x1Cu) {
            *OutChar = '\n';
            return 1u;
        }
        return 0u;
    }

    Shifted = (uint8_t)((gHalPs2ShiftLeft != 0u || gHalPs2ShiftRight != 0u) ? 1u : 0u);
    Ch = Shifted != 0u ? gHalPs2MapShift[Code] : gHalPs2Map[Code];
    if (Ch == 0) {
        return 0u;
    }

    IsLetter = (uint32_t)((Code >= 0x10u && Code <= 0x19u) ||
                          (Code >= 0x1Eu && Code <= 0x26u) ||
                          (Code >= 0x2Cu && Code <= 0x32u));
    if (IsLetter != 0u && gHalPs2CapsLock != 0u && Shifted == 0u) {
        if (Ch >= 'a' && Ch <= 'z') {
            Ch = (char)(Ch - 'a' + 'A');
        }
    } else if (IsLetter != 0u && gHalPs2CapsLock != 0u && Shifted != 0u) {
        if (Ch >= 'A' && Ch <= 'Z') {
            Ch = (char)(Ch - 'A' + 'a');
        }
    }

    *OutChar = Ch;
    return 1u;
}

static uint8_t HalpGlyph5x7Row(char Ch, uint32_t Row) {
    if (Ch >= 'a' && Ch <= 'z') {
        Ch = (char)(Ch - 'a' + 'A');
    }
    if (Ch >= '0' && Ch <= '9') {
        static const uint8_t Digits[10][7] = {
            {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
            {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
            {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
            {0x1E,0x01,0x01,0x06,0x01,0x01,0x1E},
            {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
            {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
            {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
            {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
            {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
            {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}
        };
        return Digits[(uint32_t)(Ch - '0')][Row];
    }
    switch (Ch) {
        case 'A': { static const uint8_t R[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return R[Row]; }
        case 'B': { static const uint8_t R[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return R[Row]; }
        case 'C': { static const uint8_t R[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return R[Row]; }
        case 'D': { static const uint8_t R[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return R[Row]; }
        case 'E': { static const uint8_t R[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return R[Row]; }
        case 'F': { static const uint8_t R[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return R[Row]; }
        case 'G': { static const uint8_t R[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; return R[Row]; }
        case 'H': { static const uint8_t R[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return R[Row]; }
        case 'I': { static const uint8_t R[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}; return R[Row]; }
        case 'J': { static const uint8_t R[7] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}; return R[Row]; }
        case 'K': { static const uint8_t R[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return R[Row]; }
        case 'L': { static const uint8_t R[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return R[Row]; }
        case 'M': { static const uint8_t R[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return R[Row]; }
        case 'N': { static const uint8_t R[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; return R[Row]; }
        case 'O': { static const uint8_t R[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return R[Row]; }
        case 'P': { static const uint8_t R[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return R[Row]; }
        case 'Q': { static const uint8_t R[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return R[Row]; }
        case 'R': { static const uint8_t R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return R[Row]; }
        case 'S': { static const uint8_t R[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return R[Row]; }
        case 'T': { static const uint8_t R[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return R[Row]; }
        case 'U': { static const uint8_t R[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return R[Row]; }
        case 'V': { static const uint8_t R[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return R[Row]; }
        case 'W': { static const uint8_t R[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; return R[Row]; }
        case 'X': { static const uint8_t R[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return R[Row]; }
        case 'Y': { static const uint8_t R[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return R[Row]; }
        case 'Z': { static const uint8_t R[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; return R[Row]; }
        case '[': { static const uint8_t R[7] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; return R[Row]; }
        case ']': { static const uint8_t R[7] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; return R[Row]; }
        case '>': { static const uint8_t R[7] = {0x10,0x08,0x04,0x02,0x04,0x08,0x10}; return R[Row]; }
        case '<': { static const uint8_t R[7] = {0x01,0x02,0x04,0x08,0x04,0x02,0x01}; return R[Row]; }
        case ':': { static const uint8_t R[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00}; return R[Row]; }
        case ';': { static const uint8_t R[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x08}; return R[Row]; }
        case '.': { static const uint8_t R[7] = {0x00,0x00,0x00,0x00,0x00,0x06,0x06}; return R[Row]; }
        case ',': { static const uint8_t R[7] = {0x00,0x00,0x00,0x00,0x06,0x06,0x08}; return R[Row]; }
        case '?': { static const uint8_t R[7] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}; return R[Row]; }
        case '!': { static const uint8_t R[7] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04}; return R[Row]; }
        case '=': { static const uint8_t R[7] = {0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}; return R[Row]; }
        case '+': { static const uint8_t R[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; return R[Row]; }
        case '|': { static const uint8_t R[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; return R[Row]; }
        case '_': { static const uint8_t R[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; return R[Row]; }
        case '-': { static const uint8_t R[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return R[Row]; }
        case '/': { static const uint8_t R[7] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00}; return R[Row]; }
        case '\\': { static const uint8_t R[7] = {0x10,0x08,0x04,0x02,0x01,0x00,0x00}; return R[Row]; }
        case '\'': { static const uint8_t R[7] = {0x04,0x04,0x02,0x00,0x00,0x00,0x00}; return R[Row]; }
        case '(': { static const uint8_t R[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; return R[Row]; }
        case ')': { static const uint8_t R[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; return R[Row]; }
        case ' ': return 0x00;
        default:  return 0x1Fu;
    }
}

static void HalpBuildFont8x16(void) {
    uint32_t Ch;
    uint32_t Row;

    if (gHalFontReady != 0u) {
        return;
    }

    for (Ch = 0u; Ch < 128u; Ch++) {
        for (Row = 0u; Row < 16u; Row++) {
            uint8_t RowBits = 0u;
            if (Row > 0u && Row < 15u) {
                uint32_t SrcRow = (Row - 1u) >> 1;
                uint8_t SrcBits = (uint8_t)(HalpGlyph5x7Row((char)Ch, SrcRow) & 0x1Fu);
                /* Center the 5-pixel glyph in an 8-pixel row with side padding. */
                RowBits = (uint8_t)(SrcBits << 1);
            }
            gHalFont8x16[Ch][Row] = RowBits;
        }
    }

    gHalFontReady = 1u;
}

static void HalpFbPutPixel(uint32_t X, uint32_t Y, uint32_t Color) {
    if (gHalFbReady == 0u || X >= gHalFbWidth || Y >= gHalFbHeight) {
        return;
    }
    gHalFb[(Y * gHalFbPitch) + X] = Color;
}

static void HalpFbFill(uint32_t X, uint32_t Y, uint32_t W, uint32_t H, uint32_t Color) {
    uint32_t Row;
    uint32_t Col;
    for (Row = 0u; Row < H; Row++) {
        for (Col = 0u; Col < W; Col++) {
            HalpFbPutPixel(X + Col, Y + Row, Color);
        }
    }
}

static void HalpFbClear(void) {
    if (gHalFbReady == 0u) {
        return;
    }
    HalpFbFill(0u, 0u, gHalFbWidth, gHalFbHeight, gHalFbBg);
    gHalFbCursorX = 0u;
    gHalFbCursorY = 0u;
}

static void HalpFbScroll(void) {
    uint32_t RowPixels = HALP_CONSOLE_CELL_H;
    uint32_t Y;
    uint32_t X;
    if (gHalFbReady == 0u || gHalFbHeight <= RowPixels) {
        return;
    }
    for (Y = 0u; Y + RowPixels < gHalFbHeight; Y++) {
        for (X = 0u; X < gHalFbWidth; X++) {
            gHalFb[(Y * gHalFbPitch) + X] = gHalFb[((Y + RowPixels) * gHalFbPitch) + X];
        }
    }
    HalpFbFill(0u, gHalFbHeight - RowPixels, gHalFbWidth, RowPixels, gHalFbBg);
}

static void HalpFbPutChar(char Ch) {
    uint32_t Row;
    uint32_t Col;
    uint32_t BaseX;
    uint32_t BaseY;
    if (gHalFbReady == 0u) {
        return;
    }
    if (Ch == '\r') {
        gHalFbCursorX = 0u;
        return;
    }
    if (Ch == '\n') {
        gHalFbCursorX = 0u;
        gHalFbCursorY++;
        if (gHalFbCursorY >= gHalFbRows) {
            HalpFbScroll();
            gHalFbCursorY = gHalFbRows - 1u;
        }
        return;
    }
    if (Ch == '\b') {
        uint32_t BaseX;
        uint32_t BaseY;
        if (gHalFbCursorX > 0u) {
            gHalFbCursorX--;
        } else if (gHalFbCursorY > 0u) {
            gHalFbCursorY--;
            gHalFbCursorX = gHalFbCols - 1u;
        } else {
            return;
        }
        BaseX = gHalFbCursorX * HALP_CONSOLE_CELL_W;
        BaseY = gHalFbCursorY * HALP_CONSOLE_CELL_H;
        HalpFbFill(BaseX, BaseY, HALP_CONSOLE_CELL_W, HALP_CONSOLE_CELL_H, gHalFbBg);
        return;
    }
    if ((uint8_t)Ch < 0x20u || (uint8_t)Ch > 0x7Eu) {
        Ch = '?';
    }
    if (gHalFontReady == 0u) {
        HalpBuildFont8x16();
    }
    if (gHalFbCursorX >= gHalFbCols) {
        gHalFbCursorX = 0u;
        gHalFbCursorY++;
    }
    if (gHalFbCursorY >= gHalFbRows) {
        HalpFbScroll();
        gHalFbCursorY = gHalFbRows - 1u;
    }

    BaseX = gHalFbCursorX * HALP_CONSOLE_CELL_W;
    BaseY = gHalFbCursorY * HALP_CONSOLE_CELL_H;
    HalpFbFill(BaseX, BaseY, HALP_CONSOLE_CELL_W, HALP_CONSOLE_CELL_H, gHalFbBg);

    for (Row = 0u; Row < 16u; Row++) {
        uint8_t Bits = gHalFont8x16[(uint8_t)Ch][Row];
        for (Col = 0u; Col < 8u; Col++) {
            if ((Bits & (uint8_t)(1u << (7u - Col))) != 0u) {
                HalpFbPutPixel(BaseX + Col, BaseY + Row, gHalFbColor);
            }
        }
    }

    gHalFbCursorX++;
}

static void HalpInitializeFramebuffer(void) {
    const BOOT_VIDEO_INFO *V;
    if (gHalBootInfo == 0) {
        return;
    }
    V = &gHalBootInfo->Video;
    if (V->FrameBufferBase == 0u || V->Width == 0u || V->Height == 0u || V->PixelsPerScanLine == 0u) {
        return;
    }
    gHalFb = (uint32_t *)(uintptr_t)V->FrameBufferBase;
    gHalFbWidth = V->Width;
    gHalFbHeight = V->Height;
    gHalFbPitch = V->PixelsPerScanLine;
    gHalFbBg = 0x00060A10u;
    gHalFbColor = 0x00F2F6FFu;
    gHalFbCols = (gHalFbWidth / HALP_CONSOLE_CELL_W);
    gHalFbRows = (gHalFbHeight / HALP_CONSOLE_CELL_H);
    if (gHalFbCols == 0u || gHalFbRows == 0u) {
        gHalFbReady = 0u;
        return;
    }
    HalpBuildFont8x16();
    gHalFbReady = 1u;
    HalpFbClear();
}

void HalInitializePhase0(const BOOT_INFO *BootInfo) {
    gHalBootInfo = BootInfo;

    gHalPlatformInfo.ProcessorCount = 1;
    gHalPlatformInfo.TimerFrequencyHz = 1000000;
    gHalPlatformInfo.PhysicalMemoryTop = 0;

    if (BootInfo != 0 && BootInfo->MemoryMap != 0 && BootInfo->MemoryMapEntryCount > 0) {
        const PHYSICAL_MEMORY_RANGE *Ranges;
        uint32_t Index;
        uint64_t Top = 0;

        Ranges = (const PHYSICAL_MEMORY_RANGE *)(uintptr_t)BootInfo->MemoryMap;
        for (Index = 0; Index < BootInfo->MemoryMapEntryCount; Index++) {
            uint64_t End = Ranges[Index].Base + Ranges[Index].Length;
            if (End > Top) {
                Top = End;
            }
        }
        gHalPlatformInfo.PhysicalMemoryTop = Top;
    }

    HalpInitializeSerial();
    HalpInitializeFramebuffer();
    HalpInitializeInterrupts();
    HalpInitializeTimer();
    HalWriteDebugString("[HAL] phase0 complete\n");
}

void HalInitializePhase1(void) {
    HalEnableInterrupts();
    HalWriteDebugString("[HAL] phase1 complete\n");
}

void HalQueryPlatformInfo(HAL_PLATFORM_INFO *OutInfo) {
    if (OutInfo == 0) {
        return;
    }
    *OutInfo = gHalPlatformInfo;
}

void HalRegisterTimerTickRoutine(HAL_TIMER_TICK_ROUTINE Routine) {
    gHalTimerTickRoutine = Routine;
}

void HalRegisterPageFaultRoutine(HAL_PAGE_FAULT_ROUTINE Routine) {
    gHalPageFaultRoutine = Routine;
}

void HalHandlePageFaultTrap(uint64_t FaultAddress, uint64_t ErrorCode, uint64_t InstructionPointer) {
    if (gHalPageFaultRoutine != 0) {
        gHalPageFaultRoutine(FaultAddress, ErrorCode, InstructionPointer);
        return;
    }

    HalWriteDebugString("[HAL] unhandled page fault\n");
    HalpWriteHexLine("[HAL] PF.VA=", FaultAddress);
    HalpWriteHexLine("[HAL] PF.RIP=", InstructionPointer);
    HalpWriteHexLine("[HAL] PF.ERR=", ErrorCode);
    HalHalt();
}

void HalTriggerSyntheticTimerTick(void) {
    if (gHalTimerTickRoutine != 0) {
        gHalTimerTickRoutine();
    }
}

void HalWriteDebugString(const char *Message) {
    const char *Cursor;
    if (Message == 0) {
        return;
    }

    Cursor = Message;
    while (*Cursor != 0) {
        if (*Cursor == '\n') {
            HalpSerialWriteChar('\r');
        }
        HalpSerialWriteChar(*Cursor);
        HalpFbPutChar(*Cursor);
        Cursor++;
    }
}

void HalClearDisplay(void) {
    /* Clear VGA-style framebuffer console and reset text cursor. */
    HalpFbClear();
    /* Also clear ANSI terminals attached to serial (best effort). */
    HalpSerialWriteChar('\x1B');
    HalpSerialWriteChar('[');
    HalpSerialWriteChar('2');
    HalpSerialWriteChar('J');
    HalpSerialWriteChar('\x1B');
    HalpSerialWriteChar('[');
    HalpSerialWriteChar('H');
}

uint32_t HalTryReadConsoleChar(char *OutChar) {
    if (HalpSerialTryReadChar(OutChar) != 0u) {
        return 1u;
    }
    return HalpPs2TryReadChar(OutChar);
}

void HalStallExecution(uint32_t Microseconds) {
    uint64_t Start;
    uint64_t Delta;
    uint64_t Target;

    if (Microseconds == 0) {
        return;
    }

    Start = HalReadTimestampCounter();
    Target = (uint64_t)Microseconds * 2500u;
    do {
        Delta = HalReadTimestampCounter() - Start;
    } while (Delta < Target);
}

void HalDisableInterrupts(void) {
    __asm__ __volatile__("cli");
}

void HalEnableInterrupts(void) {
    __asm__ __volatile__("sti");
}

void HalSetKernelStackPointer(uint64_t StackTop) {
    HalpSetKernelRsp0(StackTop);
}

uint64_t HalReadTimestampCounter(void) {
    uint32_t Low;
    uint32_t High;
    __asm__ __volatile__("rdtsc" : "=a"(Low), "=d"(High));
    return (((uint64_t)High) << 32) | Low;
}

void HalHalt(void) {
    HalDisableInterrupts();
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
