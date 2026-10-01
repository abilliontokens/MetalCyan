// ASRock BC-250 (AMD Cyan Skillfish): AMDRadeonX6000HWServices / HWLibs survey
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <Headers/kern_patcher.hpp>

// Acceleration survey (`-BC250FB -BC250HWL bc250hwl=N`). HWServices/HWLibs, and from level 4 the accelerator, are
// injected for the BC-250 and every named step is logged; each level lets one more step run:
//  1: HWServices/HWLibs start; createTtlInterface (builds CAIL) is blocked;
//  2: CAIL is built (HWLibs' device type and caps tables get a BC-250 entry);
//  3: the framebuffer connects to HWServices (VBIOS command tables through it);
//  4: the Navi 10 accelerator matches; newHWInterface is blocked, so it stops before any hardware object exists;
//  5: the hardware object is created and initialised up to TTL (doorbell mapped). HWLibs' debug output is on;
//  6: initializeTtl runs TTL::initialize(), CAIL's init of the GPU. It is written for the Navi 10 SMU, whose
//     message IDs the BC-250's SMU interprets differently (including voltage control): at your own risk.
//  7: bgm_create's IP version gates pass (NBIF 2.1.1, PCIE 4.2.0 and SMUIO 11.0.8 are reported as 2.3.0, 4.1.0 and
//     11.0.0; MP0/MP1 are left at 11.0.8), and TTL is stopped right after bgm_create succeeds, before any BGM
//     operation runs.
//  8: bgm_create's success is passed on: IpiValidateTopology (BGM queries, SWIP list vs. topology) runs, and
//     IpiInitializeIpInterfaces (per-SWIP GC/SDMA/PSP/SMU interfaces) is blocked. The BGM teardown's write-back to
//     VRAM offset 0 (the BC-250's framebuffer) is skipped.
//  9: IpiInitializeIpInterfaces runs (it only allocates the per-SWIP interfaces) and TlsExecuteIpEntrySeq, the first
//     call into any SWIP's init, is blocked. From this level an SMU mailbox firewall refuses every write to MP1's
//     C2PMSG registers through the SMU SWIP's register writer.
// 10: SWIPs run one at a time: sw_init (event 0) only for the first bc250swip=N (default 1) SWIPs of Navi 10's boot
//     order (BGM, GVM, PSP, SMU, GC, SDMA, MES, VCN, JPEG, DMCU), sw_fini (event 2) only for those, every other event
//     refused.
// 11: as 10, with the SMU, VCN and JPEG SWIPs stubbed: every event to them returns success without running any
//     code. The BC-250's SMU firmware is left as it runs at boot, and VCN (unusable on the BC-250) stays untouched.
// 12: every sw_init runs (bc250swip defaults to 10), and hw_init (event 1) runs for the first bc250hw=K (default 1,
//     BGM) SWIPs of the boot order, hw_fini (event 3) for those. DMCU is stubbed too. CAIL settings on the GPU turn
//     off what Linux does not do on the BC-250: SetExtendedTagEn = 0, NBIO medium-grain clock gating and light sleep
//     and ROM clock gating disabled. Every CAIL setting read is logged.
// 13: as 12 with bc250hw defaulting to (and capped at) 2: GVM's hw_init runs as a dry run. Every register write and
//     GPU memory copy it makes through its CGS wrappers is logged and not made; its reads, GPU memory allocations,
//     setting reads and golden-settings queries run and are logged. HDP memory power gating is turned off by setting.
// 14: as 13 with GVM's register writes and GPU memory copies made: the hubs' GART enable, as Linux's gfxhub/mmhub_v2_0.
//     The BC-250's FB offset (GCMC_VM_FB_OFFSET) is added to the page-table base and default/fault page addresses
//     Apple's code gives as VRAM offsets. Write64/indirect writes stay logged and not made.
// 15: as 14 with bc250hw defaulting to (and capped at) 3: PSP's hw_init runs as a dry run. Its register writes are
//     logged and not made, its reads run and are logged, and the KM ring create and every GFX command (TMR, ASD, TAs,
//     firmware loads) are logged and reported done without reaching the PSP's secure OS.
// 16: as 15 with PSP's KM ring and 4 MB TMR made, as Linux's PSP 11.0.8 does: no TOC (removed from the input), the
//     TMR only clear of the FB's first 64 MB, Linux's DESTROY_RINGS at teardown. ASD, TAs, ENABLE_INT and the Navi 10
//     firmware loads are logged and reported done without being sent.
// 17: as 16 with the firmware loads made: PSP's firmware list gets the BC-250's cyan_skillfish2 images (ME, PFP, CE,
//     MEC and its jump table, MEC2 and its jump table, RLC_G, SDMA0/1, from linux-firmware) with the SOS's type numbers;
//     Apple's other Navi 10 images (RLC restore lists, RLC P, LX6, TOC, the rest) are not loaded.
// 18: as 17 with bc250hw defaulting to (and capped at) 5: GC's hw_init runs as a dry run (the stubbed SMU answers its
//     hw_init). Every GC register write is logged and not made; its reads and golden-settings queries run and are
//     logged.
// 19: as 18 with GC's writes made: its hw_init programs GFX for real with Linux's Cyan Skillfish golden settings, the
//     RLC started as Linux does for PSP-loaded firmware, clock gating and power setup off, Navi 10's SPM sample delays
//     dropped.
// 20: as 19 with bc250hw defaulting to (and capped at) 6: SDMA's hw_init runs as a dry run. Its register writes are
//     logged and not made; the RLC autoload wait before its engine start is skipped (PSP loaded the microcode), Apple's
//     Navi 10 SDMA golden settings and SDMA clock gating are not applied, as Linux on Cyan Skillfish.
// 21: as 20 with SDMA's writes made: both engines started with Linux's sdma_v5_0 values (MIDCMD_PREEMPT on,
//     UTCL1 RESP_MODE 3, L2 cache policies DEFAULT, no SDMA_ID write).
// 22: as 21 with MES stubbed (Apple's MES hw_init waits forever for MES firmware the BC-250 does not have; the
//     accelerator schedules without MES) and bc250hw defaulting to 10, so TTL's init of the GPU completes. A
//     successful initializeTtl is then reported failed: the accelerator's own setup after it does not run yet.
// 23: as 22 with the accelerator's own setup after initializeTtl run (register bases, HW memory/VRAM info, GART, VM
//     hubs and VMM, VM registers, GFX engine and capabilities) with its register writes logged and not made; it stops
//     at initializeHWEngines (rings, KIQ, CP microcode), so Hardware::init fails there.
// 24: as 23 with the engines' init run (PM4: memory, GFX/compute/KIQ/HIQ channels; SDMA0/1; VCN2's engine removed),
//     register writes still logged and not made; a completed Hardware::init is reported failed, before the
//     accelerator starts its engines.
// 25: as 24 with Hardware::init's success passed on: the accelerator's start continues (channels, power service);
//     AMDHardware::powerUp (GFXOFF, transactions, engine power-up and start) is held back, register writes still
//     logged and not made. Every platform function asked of the accelerator is logged.
// 26: as 25 with AMDHardware::powerUp run (GFXOFF masked; VM hardware, shader memory config, engine power-up and
//     start, HW info), accelerator register writes still logged and not made.
// 27: as 26 with the accelerator's register writes made; VM page-table bases inside VRAM get the FB offset, its VM
//     TLB invalidation requests are sent in HWLibs GVM's form (Apple's are never acknowledged and hang the VM L2),
//     KIQ SET_RESOURCES, context-0 fault retry and the fault controls as Linux; CP/PSP state logged around engine start.
// 28: as 27 with the accelerator running under WindowServer (full acceleration, Metal 3): page-table entries, VM
//     programs and HIQ MAP_PROCESS page-table bases get the FB offset, GC-hub invalidations as SRBM_WRITE/POLL_REGMEM
//     (SDMA) and GVM's request form, the hardware info's and channel profiler's clocks filled, SDMA1's trap enabled and
//     its events notified with IRQMgr's dispatches, Navi 10 GPU resets blocked, only behaviour-changing hooks installed
//     (Lilu's 4 KB trampoline space). The log is also written to /private/var/log/bc250.<0|1>.log. Follow-ups:
//     compute channels run on the GFX ring (GFX1013's MEC is broken), VM contexts get Linux's fault defaults, and a
//     VM fault is logged with a page-table walk and Apple's own reports; bc250flight=1 adds the flight recorder. Needs
//     the BIOS UMA frame buffer at 4 GB (512 MB: VRAM pressure faults, green screen).
// createPowerPlayInterface (SMU) is always blocked: its Navi 10 messages do not fit the BC-250's SMU.
class BC250HWL
{
public:
    static BC250HWL& singleton();

    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);

    // Level 28: reads the IH ring beside the framebuffer's interrupt manager (from the log thread, every 0.2 s).
    void ihPoll();

    // Level 28 (28z8): the flight recorder's last GPU-memory/submission events as text; returns the length.
    size_t flightDump(char* out, size_t capacity);

    // IOGraphicsAccelerator2's orphaned-VRAM handling (from the SMU thread, every second): VRAM reuse off once the
    // accelerator exists (bc250vramreuse=1 keeps it), and a pool log every 10 calls.
    void accelPoolTick();
};
