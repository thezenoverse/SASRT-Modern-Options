// Tiny dependency-free PNG writer (uncompressed "stored" deflate blocks).
// Files are bigger than a zlib-compressed PNG, but every image viewer opens them.
#include "common.h"

static unsigned int s_crcTable[256];
static int s_crcReady = 0;

static void CrcInit(void)
{
    for (unsigned int n = 0; n < 256; n++)
    {
        unsigned int c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crcTable[n] = c;
    }
    s_crcReady = 1;
}

static unsigned int Crc(unsigned int crc, const unsigned char* data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = s_crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

typedef struct Writer
{
    HANDLE file;
    int ok;
    unsigned int crc;
} Writer;

static void Raw(Writer* w, const void* data, DWORD len)
{
    DWORD written;
    if (w->ok && (!WriteFile(w->file, data, len, &written, NULL) || written != len))
        w->ok = 0;
}

static void Be32(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void ChunkBegin(Writer* w, const char* type, unsigned int length)
{
    unsigned char hdr[8];
    Be32(hdr, length);
    memcpy(hdr + 4, type, 4);
    Raw(w, hdr, 8);
    w->crc = Crc(0, hdr + 4, 4);
}

static void ChunkData(Writer* w, const void* data, unsigned int len)
{
    Raw(w, data, len);
    w->crc = Crc(w->crc, (const unsigned char*)data, len);
}

static void ChunkEnd(Writer* w)
{
    unsigned char c[4];
    Be32(c, w->crc);
    Raw(w, c, 4);
}

int WritePngBgra(const wchar_t* path, const unsigned char* bgra, int width, int height, int stride)
{
    if (!s_crcReady)
        CrcInit();
    if (width <= 0 || height <= 0)
        return 0;

    Writer w = { 0 };
    w.file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w.file == INVALID_HANDLE_VALUE)
        return 0;
    w.ok = 1;

    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    Raw(&w, sig, 8);

    unsigned char ihdr[13];
    Be32(ihdr, (unsigned int)width);
    Be32(ihdr + 4, (unsigned int)height);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 2;  // colour type: RGB
    ihdr[10] = 0; // deflate
    ihdr[11] = 0; // adaptive filtering
    ihdr[12] = 0; // no interlace
    ChunkBegin(&w, "IHDR", 13);
    ChunkData(&w, ihdr, 13);
    ChunkEnd(&w);

    // Raw scanlines: filter byte 0 + RGB.
    size_t rowLen = 1 + (size_t)width * 3;
    size_t rawLen = rowLen * (size_t)height;
    size_t blocks = (rawLen + 65534) / 65535;
    size_t idatLen = 2 + rawLen + blocks * 5 + 4;
    if (idatLen > 0x7FFFFFFF)
    {
        CloseHandle(w.file);
        return 0;
    }

    unsigned char* row = (unsigned char*)malloc(rowLen);
    if (!row)
    {
        CloseHandle(w.file);
        return 0;
    }

    ChunkBegin(&w, "IDAT", (unsigned int)idatLen);
    static const unsigned char zhdr[2] = { 0x78, 0x01 };
    ChunkData(&w, zhdr, 2);

    unsigned int a = 1, b = 0; // adler32
    size_t remainingInBlock = 0, totalLeft = rawLen;

    for (int y = 0; y < height && w.ok; y++)
    {
        const unsigned char* src = bgra + (size_t)y * (size_t)stride;
        row[0] = 0;
        for (int x = 0; x < width; x++)
        {
            row[1 + x * 3 + 0] = src[x * 4 + 2];
            row[1 + x * 3 + 1] = src[x * 4 + 1];
            row[1 + x * 3 + 2] = src[x * 4 + 0];
        }

        size_t off = 0;
        while (off < rowLen)
        {
            if (remainingInBlock == 0)
            {
                size_t len = totalLeft < 65535 ? totalLeft : 65535;
                unsigned char bh[5];
                bh[0] = (totalLeft <= 65535) ? 1 : 0; // BFINAL on the last block, BTYPE=00 (stored)
                bh[1] = (unsigned char)(len & 0xFF);
                bh[2] = (unsigned char)(len >> 8);
                bh[3] = (unsigned char)(~len & 0xFF);
                bh[4] = (unsigned char)((~len >> 8) & 0xFF);
                ChunkData(&w, bh, 5);
                remainingInBlock = len;
            }
            size_t n = rowLen - off;
            if (n > remainingInBlock)
                n = remainingInBlock;
            ChunkData(&w, row + off, (unsigned int)n);
            for (size_t i = 0; i < n; i++)
            {
                a = (a + row[off + i]) % 65521;
                b = (b + a) % 65521;
            }
            off += n;
            remainingInBlock -= n;
            totalLeft -= n;
        }
    }

    unsigned char adler[4];
    Be32(adler, (b << 16) | a);
    ChunkData(&w, adler, 4);
    ChunkEnd(&w);

    ChunkBegin(&w, "IEND", 0);
    ChunkEnd(&w);

    free(row);
    int ok = w.ok;
    CloseHandle(w.file);
    return ok;
}
