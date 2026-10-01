// AMD Cyan Skillfish (ASRock BC-250) IP Offsets
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.
//
// Values from Linux `cyan_skillfish_ip_offset.h`. They are identical to Navi 10's.
// All values are dword register offsets into the MMIO BAR (BAR5).

#pragma once
#include <IOKit/IOTypes.h>

namespace CyanSkillfish
{
    constexpr UInt32 ATHUB_BASE_0  = 0xC00;
    constexpr UInt32 GC_BASE_0     = 0x1260;
    constexpr UInt32 GC_BASE_1     = 0xA000;
    constexpr UInt32 HDP_BASE_0    = 0xF20;
    constexpr UInt32 MMHUB_BASE_0  = 0x1A000;
    constexpr UInt32 MP0_BASE_0    = 0x16000;
    constexpr UInt32 MP1_BASE_0    = 0x16000;
    constexpr UInt32 NBIO_BASE_0   = 0x0;
    constexpr UInt32 NBIO_BASE_2   = 0xD20;
    constexpr UInt32 OSSSYS_BASE_0 = 0x10A0;
    constexpr UInt32 SMUIO_BASE_0  = 0x16800;
    constexpr UInt32 DCN_BASE_2    = 0x34C0;
    constexpr UInt32 MP1_PUBLIC    = 0x3B00000;    // SMN aperture
}    // namespace CyanSkillfish
