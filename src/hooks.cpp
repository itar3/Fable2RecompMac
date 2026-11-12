/**
 * @file        src/hooks.cpp
 *
 * @brief       Function hooks and replacements for guest module runtime.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "generated/Fable2_init.h"
#include <rex/hook.h>

namespace {

// GetFileSectorInfo replacement — the game queries disc sector positions to
// optimize read ordering. return 0 (no sector info).
u32 GetFileSectorInfo_entry(u32 handle) {
    return 0;
}

}  // namespace

REX_HOOK(sub_82CC7BB8, GetFileSectorInfo_entry)
