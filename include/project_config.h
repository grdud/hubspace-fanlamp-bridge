#pragma once
#if __has_include("bridge_config.h")
#include "bridge_config.h"
#else
#include "bridge_config.example.h"
#endif
static_assert(BRIDGE_SWITCH_1_MIN>=1 && BRIDGE_SWITCH_1_MIN<100,"Switch 1 minimum must be 1..99");
static_assert(BRIDGE_SWITCH_2_MIN>=1 && BRIDGE_SWITCH_2_MIN<100,"Switch 2 minimum must be 1..99");
static_assert(BRIDGE_BLE_INDEX>=0 && BRIDGE_BLE_INDEX<=255,"BLE index must be a byte");
