/* verified_rom.h — TLK ROM verifier */
#ifndef TLK_VERIFIED_ROM_H
#define TLK_VERIFIED_ROM_H

#include <stddef.h>
#include <stdint.h>

/* Read and verify the Lion King (USA) ROM.  Returns a malloc'd buffer
 * on success (caller frees), NULL on failure with a message in error[].
 * Strips a 512-byte copier header if present.  Validates SHA-256. */
uint8_t *TlkReadVerifiedRom(const char *path, size_t *size_out,
                            char *error, size_t error_size);

#endif /* TLK_VERIFIED_ROM_H */
