#include "d99_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* little/big-endian scalar readers */
static uint32_t e_rd32(const unsigned char *p, int be)
{
    if (be)
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

static uint64_t e_rd64(const unsigned char *p, int be)
{
    uint64_t v = 0;
    int i;
    if (be) {
        for (i = 0; i < 8; i++)
            v = (v << 8) | p[i];
    } else {
        for (i = 7; i >= 0; i--)
            v = (v << 8) | p[i];
    }
    return v;
}

static uint16_t e_rd16(const unsigned char *p, int be)
{
    if (be)
        return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

int d99_elf_is_elf(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char magic[4];
    int ok = 0;

    if (!f)
        return 0;
    if (fread(magic, 1, 4, f) == 4 &&
        magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')
        ok = 1;
    fclose(f);
    return ok;
}

int d99_elf_needed(const char *path, d99_strvec *out)
{
    size_t len;
    unsigned char *buf = (unsigned char *)d99_read_file(path, &len);
    int is64, be;
    uint64_t phoff;
    uint16_t phentsize, phnum;
    uint16_t i;
    uint64_t strtab_vaddr = 0, strsz = 0;
    unsigned char *dyn = NULL;
    uint64_t dyn_size = 0;

    if (!buf)
        return -1;
    if (len < 16 || memcmp(buf, "\x7f" "ELF", 4) != 0) {
        free(buf);
        return 1;
    }
    is64 = (buf[4] == 2);
    be = (buf[5] == 2);

    if (is64) {
        if (len < 64) {
            free(buf);
            return -1;
        }
        phoff = e_rd64(buf + 0x20, be);
        phentsize = e_rd16(buf + 0x36, be);
        phnum = e_rd16(buf + 0x38, be);
    } else {
        if (len < 52) {
            free(buf);
            return -1;
        }
        phoff = e_rd32(buf + 0x1c, be);
        phentsize = e_rd16(buf + 0x2a, be);
        phnum = e_rd16(buf + 0x2c, be);
    }
    if (phentsize == 0 || phnum == 0 ||
        phoff + (uint64_t)phentsize * phnum > len) {
        free(buf);
        return -1;
    }

    for (i = 0; i < phnum; i++) {
        unsigned char *ph = buf + phoff + (uint64_t)i * phentsize;
        uint32_t ptype = e_rd32(ph, be);
        if (ptype == 2) {   /* PT_DYNAMIC */
            if (is64) {
                dyn = buf + e_rd64(ph + 0x08, be);
                dyn_size = e_rd64(ph + 0x20, be);
            } else {
                dyn = buf + e_rd32(ph + 0x04, be);
                dyn_size = e_rd32(ph + 0x10, be);
            }
            break;
        }
    }
    if (!dyn) {
        free(buf);
        return 0;   /* statically linked or no dynamic section */
    }
    if ((size_t)(dyn - buf) >= len) {
        free(buf);
        return -1;
    }

    /* walk dynamic entries */
    {
        size_t esz = is64 ? 16 : 8;
        uint64_t strtab_off = 0;
        size_t count = (size_t)(dyn_size / esz);
        size_t k;

        if ((size_t)(dyn - buf) + count * esz > len)
            count = (len - (size_t)(dyn - buf)) / esz;
        /* first pass: DT_STRTAB vaddr */
        for (k = 0; k < count; k++) {
            unsigned char *e = dyn + k * esz;
            uint64_t tag = is64 ? e_rd64(e, be) : e_rd32(e, be);
            uint64_t val = is64 ? e_rd64(e + 8, be) : e_rd32(e + 4, be);
            if (tag == 0)
                break;   /* DT_NULL */
            if (tag == 5)
                strtab_vaddr = val;
            if (tag == 10)
                strsz = val;
        }
        /* map vaddr -> file offset using PT_LOAD segments */
        for (i = 0; i < phnum; i++) {
            unsigned char *ph = buf + phoff + (uint64_t)i * phentsize;
            uint32_t ptype = e_rd32(ph, be);
            if (ptype == 1) {   /* PT_LOAD */
                uint64_t vaddr = is64 ? e_rd64(ph + 0x10, be)
                                      : e_rd32(ph + 0x08, be);
                uint64_t filesz = is64 ? e_rd64(ph + 0x20, be)
                                       : e_rd32(ph + 0x10, be);
                uint64_t off = is64 ? e_rd64(ph + 0x08, be)
                                    : e_rd32(ph + 0x04, be);
                if (strtab_vaddr >= vaddr && strtab_vaddr < vaddr + filesz) {
                    strtab_off = strtab_vaddr - vaddr + off;
                    break;
                }
            }
        }
        if (strtab_off == 0 || strtab_off >= len) {
            free(buf);
            return 0;   /* no valid string table (e.g. static) */
        }
        /* second pass: DT_NEEDED */
        for (k = 0; k < count; k++) {
            unsigned char *e = dyn + k * esz;
            uint64_t tag = is64 ? e_rd64(e, be) : e_rd32(e, be);
            uint64_t val = is64 ? e_rd64(e + 8, be) : e_rd32(e + 4, be);
            if (tag == 0)
                break;
            if (tag == 1) {   /* DT_NEEDED */
                if (val < strsz && strtab_off + val < len) {
                    const char *so = (const char *)buf + strtab_off + val;
                    if (so[0])
                        d99_sv_push(out, so);
                }
            }
        }
    }
    free(buf);
    return 0;
}
