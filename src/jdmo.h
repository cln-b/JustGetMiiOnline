#pragma once

#include <cstdint>

bool jdmo_supported(uint64_t title);
void jdmo_begin();
void jdmo_apply();
void jdmo_end();
const char *jdmo_status();
uint32_t jdmo_swaps();
