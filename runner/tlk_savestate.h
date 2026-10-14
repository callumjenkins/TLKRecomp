#pragma once
#include <stdint.h>
/* Savestates between frames (task S). Return 1 on success. */
int tlk_state_save(const char *path, uint32_t host_frame);
int tlk_state_load(const char *path, uint32_t *host_frame);
