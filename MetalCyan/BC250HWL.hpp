// ASRock BC-250 (AMD Cyan Skillfish): AMDRadeonX6000HWServices, HWLibs and the accelerator
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <Headers/kern_patcher.hpp>

// Apple's Navi 10 driver (HWServices, HWLibs' TTL/CAIL and the accelerator) on the BC-250: HWLibs' tables and IP
// version gates take the BC-250, its SWIPs run with the SMU, VCN, JPEG, DMCU and MES stubbed, PSP loads the
// cyan_skillfish2 microcode, GVM/GC/SDMA are programmed as Linux's amdgpu does on Cyan Skillfish (FB offset, golden
// settings, RLC start, no clock gating), and the accelerator's VM, invalidation, reset and VCN paths are fixed up
// for it. PowerPlay is blocked; BC250Smu drives the SMU. Needs the BIOS UMA frame buffer at 4 GB.
class BC250HWL
{
public:
    static BC250HWL& singleton();

    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);

    // Keeps SDMA1's trap enabled as SDMA0's (from the poll thread, every 0.2 s).
    void sdma1TrapPoll();

    // IOGraphicsAccelerator2's orphaned-VRAM handling (from the SMU thread, every second): VRAM reuse off once the
    // accelerator exists (bc250vramreuse=1 keeps it).
    void accelPoolTick();
};
