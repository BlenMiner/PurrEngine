#include "tide/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/net.h"

// See tide/page.h.

_Noreturn void tide_out_of_memory(void)
{
    fprintf(stderr, "tide: out of memory\n");
    abort();
}

tide_page *tide_page_new(const uint32_t size)
{
    tide_page *p = calloc(1, sizeof *p + size);
    if (!p) tide_out_of_memory();
    p->refs = 1;
    p->size = size;
    p->hashed = UINT32_MAX;
    return p;
}

void tide_page_release(tide_page *p, const uint32_t refs)
{
    if (!p) return;
    p->refs -= refs;
    if (p->refs == 0) free(p);
}

tide_page *tide_page_own(tide_page *p, const uint32_t mine, const uint32_t keep)
{
    p->hashed = UINT32_MAX;
    if (p->refs == mine) return p;
    tide_page *copy = malloc(sizeof *copy + p->size);
    if (!copy) tide_out_of_memory();
    const uint32_t kept = keep < p->size ? keep : p->size;
    *copy = (tide_page){mine, p->size, p->first, UINT32_MAX, 0, 0};
    memcpy(tide_page_data(copy), tide_page_data(p), kept);
    memset((uint8_t *)tide_page_data(copy) + kept, 0, p->size - kept);
    p->refs -= mine;
    return copy;
}

uint64_t tide_page_hash(tide_page *p, const uint32_t bytes)
{
    if (p->hashed != bytes) {
        p->hash = tide_hash(tide_page_data(p), bytes);
        p->hashed = bytes;
    }
    return p->hash;
}
