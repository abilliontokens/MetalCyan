// AMD Cyan Skillfish (ASRock BC-250) Registers
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.
//
// Absolute dword offsets (IP base + register offset), from Linux `gc_10_1_0_offset.h`,
// `nbio_2_3_offset.h`, `mp_11_0_offset.h` and `amdgpu.h`.

#pragma once
#include <GPUDriversAMD/CyanSkillfishIPOffset.hpp>

namespace CyanSkillfish
{
    // BIF indirect VRAM access (amdgpu.h: mmMM_INDEX/mmMM_DATA/mmMM_INDEX_HI).
    constexpr UInt32 MM_INDEX    = 0x0;
    constexpr UInt32 MM_DATA     = 0x1;
    constexpr UInt32 MM_INDEX_HI = 0x6;

    // NBIO 2.3 (BASE_IDX 0 unless noted).
    constexpr UInt32 PCIE_INDEX2                          = NBIO_BASE_0 + 0xE;
    constexpr UInt32 PCIE_DATA2                           = NBIO_BASE_0 + 0xF;
    constexpr UInt32 RCC_DEV0_EPF0_STRAP0                 = NBIO_BASE_2 + 0x11;
    constexpr UInt32 RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_MASK = 0x0F000000;
    constexpr UInt32 RCC_DEV0_EPF0_STRAP0_ATI_REV_ID_SHIFT = 24;
    constexpr UInt32 RCC_CONFIG_MEMSIZE                   = NBIO_BASE_2 + 0xC3;    // Carve-out size in MiB.

    // GC 10.1 (BASE_IDX 0).
    constexpr UInt32 GRBM_STATUS                   = GC_BASE_0 + 0xDA4;
    constexpr UInt32 CC_GC_SHADER_ARRAY_CONFIG     = GC_BASE_0 + 0x100F;
    constexpr UInt32 GC_USER_SHADER_ARRAY_CONFIG   = GC_BASE_0 + 0x1010;
    constexpr UInt32 SHADER_ARRAY_INACTIVE_WGPS_SHIFT = 16;
    constexpr UInt32 CC_RB_BACKEND_DISABLE         = GC_BASE_0 + 0x13DD;
    constexpr UInt32 GB_ADDR_CONFIG                = GC_BASE_0 + 0x13DE;
    constexpr UInt32 GCMC_VM_FB_OFFSET             = GC_BASE_0 + 0x170B;    // Carve-out system address >> 24.
    constexpr UInt32 GCMC_VM_FB_LOCATION_BASE      = GC_BASE_0 + 0x1720;    // MC address >> 24.
    constexpr UInt32 GCMC_VM_FB_LOCATION_TOP       = GC_BASE_0 + 0x1721;
    constexpr UInt32 GCMC_VM_AGP_TOP               = GC_BASE_0 + 0x1722;
    constexpr UInt32 GCMC_VM_AGP_BOT               = GC_BASE_0 + 0x1723;
    constexpr UInt32 GCMC_VM_AGP_BASE              = GC_BASE_0 + 0x1724;

    // Golden GB_ADDR_CONFIG for GC 10.1.3 (gfx_v10_0.c).
    constexpr UInt32 GB_ADDR_CONFIG_GOLDEN = 0x00100044;

    // MP0 (PSP) mailbox.
    constexpr UInt32 MP0_SMN_C2PMSG_33  = MP0_BASE_0 + 0x61;    // Bit 31: IFWI init done.
    constexpr UInt32 MP0_SMN_C2PMSG_58  = MP0_BASE_0 + 0x7A;    // SOS/TOS version.
    constexpr UInt32 MP0_SMN_C2PMSG_81  = MP0_BASE_0 + 0x91;    // SOS status.
    constexpr UInt32 MP0_SMN_C2PMSG_100 = MP0_BASE_0 + 0xA4;    // Bootloader version.

    // MP1 (SMU 11) mailbox. Read-only use here: never write C2PMSG_66 (message) from the probe.
    constexpr UInt32 MP1_SMN_C2PMSG_66 = MP1_BASE_0 + 0x282;    // Message.
    constexpr UInt32 MP1_SMN_C2PMSG_82 = MP1_BASE_0 + 0x292;    // Argument.
    constexpr UInt32 MP1_SMN_C2PMSG_90 = MP1_BASE_0 + 0x29A;    // Response.

    // SMN addresses (byte addresses via PCIE_INDEX2/PCIE_DATA2).
    constexpr UInt32 SMN_MP1_FIRMWARE_FLAGS                 = MP1_PUBLIC | 0x3010024;
    constexpr UInt32 MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED  = 0x1;

    // IP discovery (amdgpu_discovery.h).
    constexpr UInt32 DISCOVERY_TMR_OFFSET = 64 << 10;    // From the end of the carve-out.
    constexpr UInt32 DISCOVERY_TMR_SIZE   = 10 << 10;
}    // namespace CyanSkillfish
