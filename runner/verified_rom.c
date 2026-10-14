/* verified_rom.c — Read and verify the Lion King (USA) ROM. */
#include "verified_rom.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lion King, The (USA).sfc  headerless HiROM, 3 MiB.
 * SHA-256: 457abe634e0a8be03e29513a3dca8f3e9d0ddc6bf97d8931f2316094260f3712 */
static const uint8_t kExpectedSha256[32] = {
    0x45, 0x7a, 0xbe, 0x63, 0x4e, 0x0a, 0x8b, 0xe0,
    0x3e, 0x29, 0x51, 0x3a, 0x3d, 0xca, 0x8f, 0x3e,
    0x9d, 0x0d, 0xdc, 0x6b, 0xf9, 0x7d, 0x89, 0x31,
    0xf2, 0x31, 0x60, 0x94, 0x26, 0x0f, 0x37, 0x12,
};

static void SetError(char *error, size_t error_size, const char *msg) {
    if (error && error_size > 0)
        (void)snprintf(error, error_size, "%s", msg);
}

static void SetUnsupported(char *error, size_t error_size, size_t size,
                           const uint8_t hash[32]) {
    if (!error || error_size == 0) return;
    int w = snprintf(error, error_size,
                     "unsupported ROM (size=%zu sha256=", size);
    if (w < 0 || (size_t)w >= error_size) return;
    size_t used = (size_t)w;
    for (size_t i = 0; i < 32 && used + 2 < error_size; i++) {
        w = snprintf(error + used, error_size - used, "%02x", hash[i]);
        if (w != 2) return;
        used += 2;
    }
    if (used + 2 <= error_size) {
        error[used++] = ')';
        error[used] = '\0';
    }
}

uint8_t *TlkReadVerifiedRom(const char *path, size_t *size_out,
                            char *error, size_t error_size) {
    if (size_out) *size_out = 0;
    if (!path || !*path || !size_out) {
        SetError(error, error_size, "invalid ROM path or output pointer");
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        SetError(error, error_size, "unable to open ROM");
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        SetError(error, error_size, "unable to seek ROM");
        return NULL;
    }
    long length = ftell(f);
    if (length <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        SetError(error, error_size, "ROM is empty or unreadable");
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)length);
    if (!buf) {
        fclose(f);
        SetError(error, error_size, "out of memory");
        return NULL;
    }
    if (fread(buf, 1, (size_t)length, f) != (size_t)length) {
        free(buf);
        fclose(f);
        SetError(error, error_size, "unable to read ROM");
        return NULL;
    }
    fclose(f);

    /* Strip copier header if present. */
    size_t skip = ((size_t)length % 1024u == 512u) ? 512u : 0u;
    size_t payload = (size_t)length - skip;
    if (skip) memmove(buf, buf + skip, payload);

    uint8_t hash[32];
    sha256_compute(buf, payload, hash);
    if (payload != 0x300000u ||
        memcmp(hash, kExpectedSha256, sizeof hash) != 0) {
        SetUnsupported(error, error_size, payload, hash);
        free(buf);
        return NULL;
    }

    *size_out = payload;
    if (error && error_size) error[0] = '\0';
    return buf;
}
