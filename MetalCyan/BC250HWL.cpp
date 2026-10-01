// ASRock BC-250 (AMD Cyan Skillfish): AMDRadeonX6000HWServices / HWLibs survey
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <BC250.hpp>
#include <BC250HWL.hpp>
#include <BC250Smu.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread.h>
#include <GPUDriversAMD/CAIL/ASICCaps.hpp>
#include <GPUDriversAMD/CAIL/DeviceType.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <Headers/kern_mach.hpp>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>

static BC250HWL moduleInstance;

BC250HWL& BC250HWL::singleton() { return moduleInstance; }

namespace
{
    enum class Kext : UInt8
    {
        FB,
        HWServices,
        HWLibs,
        Accel,
    };

    // Hooks on Apple's functions that either block a step (Navi 10 paths that would hang or misprogram the BC-250) or
    // change something around it; all share one pass-through (wrap<I>).
    struct Hook
    {
        Kext              kext;
        const char*       name;
        const char*       symbol;
        bool              block;    // Return 0 without running Apple's function.
        mach_vm_address_t org;
        UInt32            calls;
    };

    Hook hooks[] = {
        // PowerPlay talks to the SMU with Navi 10's message set, which the BC-250's SMU 11.0.8 reads differently.
        {Kext::HWServices, "HWServices::createPowerPlayInterface",
            "__ZN42AMDRadeonX6000_AMDRadeonHWServicesAbstract24createPowerPlayInterfaceEP18PowerPlayCallbacks", true, 0,
            0},
        {Kext::HWLibs, "HWLibs::createPowerPlayInterface",
            "__ZN30AMDRadeonX6000_AMDRadeonHWLibs24createPowerPlayInterfaceEP18PowerPlayCallbacks", true, 0, 0},
        {Kext::Accel, "Accelerator::start", "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService", false, 0, 0},
        {Kext::Accel, "Hardware::init",
            "__ZN26AMDRadeonX6000_AMDHardware4initEP11IOPCIDeviceP28AMDRadeonX6000_IAMDHWHandlerRjP16_GART_"
            "PARAMETERSP14_FB_PARAMETERS",
            false, 0, 0},
        {Kext::Accel, "RTHardware::initializeTtl",
            "__ZN28AMDRadeonX6000_AMDRTHardware13initializeTtlEP16_GART_PARAMETERS", false, 0, 0},
        {Kext::Accel, "Navi10Hardware::allocateHWEngines", "__ZN32AMDRadeonX6000_AMDNavi10Hardware17allocateHWEnginesEv",
            false, 0, 0},
        {Kext::Accel, "Hardware::powerUp", "__ZN26AMDRadeonX6000_AMDHardware7powerUpEv", false, 0, 0},
        // Apple's hang recovery reads debug/queue state and resets GPU blocks the Navi 10 way, which hangs the BC-250.
        {Kext::Accel, "AccelChannel::resetHardwareAndReplay",
            "__ZN30AMDRadeonX6000_AMDAccelChannel22resetHardwareAndReplayEv", true, 0, 0},
        {Kext::Accel, "Hardware::dumpASICHangState", "__ZN26AMDRadeonX6000_AMDHardware17dumpASICHangStateEb", true, 0, 0},
        {Kext::Accel, "GFX10Hardware::asicHangReadROQandMEQStatus",
            "__ZN31AMDRadeonX6000_AMDGFX10Hardware27asicHangReadROQandMEQStatusER27amdAsicHangDumpROQMEQStatus", true, 0,
            0},
        {Kext::Accel, "Hardware::resetHardware", "__ZN26AMDRadeonX6000_AMDHardware13resetHardwareEjPj", true, 0, 0},
        {Kext::Accel, "GFX10PM4Engine::cpSoftReset", "__ZN32AMDRadeonX6000_AMDGFX10PM4Engine11cpSoftResetEv", true, 0, 0},
        {Kext::Accel, "GFX10PM4Engine::gfxEngineReset", "__ZN32AMDRadeonX6000_AMDGFX10PM4Engine14gfxEngineResetEv", true,
            0, 0},
        // SDMA1's interrupt events (see sdma1Kick).
        {Kext::FB, "Framebuffer::registerForInterruptType",
            "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer24registerForInterruptTypeEjPFvP8OSObjectPvES1_S2_PS2_", false, 0,
            0},
        {Kext::FB, "InterruptEvent::notifyCallback", "__ZN32AMDRadeonX6000_AmdInterruptEvent14notifyCallbackEPv", false,
            0, 0},
    };

    bool amdKextsLoaded = false;    // Set when the first AMD kext loads; the GPU is up from then on.
    bool   accelInStart = false;    // Inside AMDGraphicsAccelerator::start (its failure path calls stop).
    bool   accelStopUnguarded = false;
    constexpr UInt32 kVcnEngineSlot = 0x3F0;    // AMDNavi10Hardware's VCN2 engine (allocateHWEngines).
    constexpr UInt32 kEngineSlot5 = 0x3B0 + 5 * 8, kHwCapabilities = 0x158, kHwGfxOffSupported = 0x206C0;
    bool   accelHeldBack = false;    // Level 23 without its register hooks: stop after initializeTtl as level 22.

    const char* className(const void* object)
    {
        if (object == nullptr) { return "null"; }
        const auto* meta = static_cast<const OSMetaClassBase*>(object)->getMetaClass();
        return meta != nullptr ? meta->getClassName() : "?";
    }

    void        gcCpBrief(const char* when);
    const char* kextOf(mach_vm_address_t a, mach_vm_address_t* offset);
    void        hwChannelsDump(const char* when);

    // Level 28: the hardware info the accelerator hands the Metal driver (AMDHardware +0xD0, 0x204 bytes, hardware
    // vtable +0x1C0). AMDHardware::initHWInfo takes its clocks (+0x190 reference, +0x198 system, +0x1A0 memory, +0x1A8
    // CG reference, in 10 kHz units: GFX10's updateHWInfoClocks writes 208100 for a 2081 MHz part) from TTL's clock
    // query (TTL vtable +0x80), which fails without PowerPlay/SMU. AMDAccelDevice::getHardwareInfo then refuses the
    // info ("Invalid values"), the Metal device is not created and WindowServer aborts ("Failed to create
    // MetalDevice"). Zero clocks get the BC-250's: 100 MHz reference (GFX10's xclk, the GPU timestamp clock), 2000 MHz
    // GFX (cyan_skillfish's maximum), 1750 MHz GDDR6 (14 Gbps).
    //
    // Filling them in the hardware object (bc250clk=2) hung the machine within 0.2 s, during the accelerator's own
    // start, before WindowServer (28b-28d; with the fill off the same build stays up): something in the kernel acts on
    // non-zero clocks. By default (bc250clk=1) the kernel keeps Apple's zeros and only getHardwareInfo's copy for the
    // Metal driver gets the BC-250's clocks (wrapAccelHwInfo). bc250clk=0: neither.
    constexpr UInt64 kBc250RefClk = 10000, kBc250SysClk = 200000, kBc250MemClk = 175000;
    SInt32           hwInfoClockMode = -1;

    UInt32 hwInfoClkMode()
    {
        if (hwInfoClockMode < 0) {
            UInt32 clk = 1;
            PE_parse_boot_argn("bc250clk", &clk, sizeof(clk));
            hwInfoClockMode = static_cast<SInt32>(clk);
            BCLOG("BC250HWL", "level 28: clocks for the Metal driver: %s", clk == 0 ? "none (Apple's zeros)" :
                                                                        clk == 2 ? "in the hardware object" :
                                                                                   "in getHardwareInfo's copy only");
        }
        return static_cast<UInt32>(hwInfoClockMode);
    }

    // 28u: the registrations (framebuffer registerForInterruptType): the handle it returns is {event, callback}; the
    // callback (AmdInterruptCallback) is +0x18 function, +0x20 owner, +0x30 enabled, +0x58 reference.
    struct IrqRegistration
    {
        UInt32 type;
        void*  event;
        void*  callback;
    };
    IrqRegistration irqRegs[128];
    UInt32          irqRegCount = 0;

    void irqRegistered(UInt32 type, void** handle)
    {
        if (handle == nullptr || irqRegCount >= arrsize(irqRegs)) { return; }
        irqRegs[irqRegCount++] = {type, handle[0], handle[1]};
    }

    // 28y: SDMA1's traps (IH client 9, source 0xE0) reach the ring once its TRAP_ENABLE is set (28x), but IRQMgr never
    // dispatches its SDMA1 sources (IRQ_SOURCEX_SDMA1_TRAP_GFX/PAGE, 0xFF0003E8/9): SDMA1's fences complete unseen until
    // IOAccel's 6 s progress check. Each time IRQMgr dispatches any event (at least the display's V_UPDATE, ~60 Hz),
    // SDMA1's events are notified too, in the same context: HWChannel::timeStampInterruptCallback reads the fence itself
    // and returns at once when nothing new completed (bc250sdma1kick=0 disables).
    // SDMA1's events, notified with every other IRQMgr dispatch (below) and from a timer (bug 4: while the display is
    // idle or being reprogrammed there is no V_UPDATE, so SDMA1's finished work waited for IOAccel's 5 s progress
    // check: slow lock screen, mode sets, boot). The timer checks the SDMA1 channels (the callbacks' owners) for
    // submitted-but-unseen work every 10 ms, every 1 ms while there is. It runs on a workloop of its own: on the
    // framebuffer's, it could not fire while the display bring-up waited on SDMA1 holding that workloop's gate (a
    // 12 s stall at boot); IRQMgr itself dispatches from several threads.
    mach_vm_address_t   sdma1NotifyOrg = 0;
    volatile UInt32     sdma1KickBusy  = 0;
    SInt32              sdma1KickOn    = -1;
    IOTimerEventSource* sdma1Timer     = nullptr;
    IOWorkLoop*         sdma1WorkLoop  = nullptr;
    UInt32              sdma1Kicks = 0, sdma1TimerKicks = 0;

    bool sdma1KickEnabled()
    {
        if (sdma1KickOn < 0) {
            UInt32 kick = 1;
            PE_parse_boot_argn("bc250sdma1kick", &kick, sizeof(kick));
            sdma1KickOn = kick != 0 ? 1 : 0;
        }
        return sdma1KickOn == 1;
    }

    bool sdma1Pending()
    {
        for (UInt32 r = 0; r < irqRegCount; r++) {
            if (irqRegs[r].type != 0xFF0003E8 && irqRegs[r].type != 0xFF0003E9) { continue; }
            void* callback = irqRegs[r].callback;
            void* channel  = callback ? getMember<void*>(callback, 0x20) : nullptr;
            if (channel != nullptr && getMember<UInt32>(channel, 0x80) != getMember<UInt32>(channel, 0x84)) {
                return true;
            }
        }
        return false;
    }

    void sdma1Kick(mach_vm_address_t org, void* current, void* info)
    {
        if (org != 0) { sdma1NotifyOrg = org; }
        if (!sdma1KickEnabled() || sdma1NotifyOrg == 0 || !OSCompareAndSwap(0, 1, &sdma1KickBusy)) { return; }
        void* done[2] = {nullptr, nullptr};
        for (UInt32 r = 0; r < irqRegCount; r++) {
            if (irqRegs[r].type != 0xFF0003E8 && irqRegs[r].type != 0xFF0003E9) { continue; }
            void* event = irqRegs[r].event;
            if (event == nullptr || event == current || event == done[0] || event == done[1]) { continue; }
            if (done[0] == nullptr) {
                done[0] = event;
            }
            else {
                done[1] = event;
            }
            reinterpret_cast<UInt64 (*)(void*, void*)>(sdma1NotifyOrg)(event, info);
            if (sdma1Kicks++ == 0) {
                BCLOG("BC250HWL", "SDMA1 events notified with IRQMgr's dispatches (first: event %p, type 0x%08X)", event,
                    irqRegs[r].type);
            }
        }
        sdma1KickBusy = 0;
    }

    void sdma1TimerFired(OSObject*, IOTimerEventSource* timer)
    {
        const bool pending = sdma1Pending();
        if (pending) {
            sdma1Kick(0, nullptr, nullptr);
            if (sdma1TimerKicks++ == 0) { BCLOG("BC250HWL", "SDMA1 events notified from the workloop timer (first)"); }
        }
        timer->setTimeoutUS(pending ? 1000 : 10000);
    }

    mach_vm_address_t orgStartInterrupts = 0;

    UInt64 wrapStartInterrupts(void* manager, IOWorkLoop* workLoop)
    {
        const UInt64 ret = FunctionCast(wrapStartInterrupts, orgStartInterrupts)(manager, workLoop);
        UInt32       timer = 1;
        PE_parse_boot_argn("bc250sdma1timer", &timer, sizeof(timer));
        if (timer != 0 && sdma1KickEnabled() && workLoop != nullptr && sdma1Timer == nullptr) {
            sdma1WorkLoop = IOWorkLoop::workLoop();
            sdma1Timer    = IOTimerEventSource::timerEventSource(static_cast<OSObject*>(manager), sdma1TimerFired);
            if (sdma1WorkLoop != nullptr && sdma1Timer != nullptr &&
                sdma1WorkLoop->addEventSource(sdma1Timer) == kIOReturnSuccess)
            {
                sdma1Timer->setTimeoutMS(10);
                BCLOG("BC250HWL", "level 28: SDMA1 notify timer on its own workloop %p", sdma1WorkLoop);
            }
            else {
                BCLOG("BC250HWL", "level 28: SDMA1 notify timer NOT started");
                OSSafeReleaseNULL(sdma1Timer);
                OSSafeReleaseNULL(sdma1WorkLoop);
            }
        }
        return ret;
    }

    enum : UInt32
    {
        kWrapRun   = 1,
        kWrapStart = 2,
        kWrapQuiet = 4,
    };

    __attribute__((noinline)) UInt32 wrapBefore(Hook& hook, void* a, void* b, void*)
    {
        hook.calls += 1;
        if (hook.block) { return 0; }
        if (strcmp(hook.name, "InterruptEvent::notifyCallback") == 0) {
            sdma1Kick(hook.org, a, b);
            return kWrapRun | kWrapQuiet;
        }
        // GFXOFF stays off (hardware +0x206c0 selects Apple's GFXOFF requests to the SMU, which is a stub; Linux keeps
        // GFXOFF off on the BC-250).
        if (strcmp(hook.name, "Hardware::powerUp") == 0) { getMember<UInt8>(a, kHwGfxOffSupported) = 0; }
        const bool start = strcmp(hook.name, "Accelerator::start") == 0;
        if (start) { accelInStart = true; }
        return kWrapRun | (start ? kWrapStart : 0);
    }

    // VCN (video decode/encode) is a stub on the BC-250, but Apple's Navi 10 accelerator personality advertises it
    // (IOGVACodec AMDVCN2, IOGVAHEVCDecode/Encode + capabilities, H.264 encode, IOVARendererID, IODVDBundleName):
    // VideoToolbox then picks the AMD decoder, AppleGVA fails to create it (AVD_api.Create err=5) and Safari plays no
    // video. Without them macOS decodes in software. bc250vcnprops=1 keeps them.
    void removeVideoProperties(void* accelerator)
    {
        UInt32 keep = 0;
        PE_parse_boot_argn("bc250vcnprops", &keep, sizeof(keep));
        auto* service = OSDynamicCast(IOService, static_cast<OSObject*>(accelerator));
        if (keep != 0 || service == nullptr) { return; }
        OSDictionary* props = service->dictionaryWithProperties();
        if (props == nullptr) { return; }
        OSArray* names = OSArray::withCapacity(16);
        if (auto* iter = OSCollectionIterator::withCollection(props); iter != nullptr) {
            while (auto* key = OSDynamicCast(OSSymbol, iter->getNextObject())) {
                const char* name = key->getCStringNoCopy();
                if (strncmp(name, "IOGVA", 5) == 0 || strcmp(name, "IOVARendererID") == 0 ||
                    strcmp(name, "IOVARendererSubID") == 0 || strcmp(name, "IODVDBundleName") == 0) {
                    if (names != nullptr) { names->setObject(key); }
                }
            }
            iter->release();
        }
        props->release();
        if (names == nullptr) { return; }
        for (unsigned i = 0; i < names->getCount(); i++) {
            const auto* key = OSDynamicCast(OSSymbol, names->getObject(i));
            if (key == nullptr) { continue; }
            service->removeProperty(key);
            BCLOG("BC250HWL", "level 28: accelerator property %s removed (VCN is a stub: software video decode)",
                key->getCStringNoCopy());
        }
        names->release();
    }

    __attribute__((noinline)) UInt64 wrapAfter(Hook& hook, UInt32 flags, UInt64 ret, void* a, void* b, void*, void* f, void*)
    {
        if (flags & kWrapQuiet) { return ret; }
        if (flags & kWrapStart) {
            accelInStart = false;
            if ((ret & 0xFF) != 0) { removeVideoProperties(a); }
        }
        if (strcmp(hook.name, "Framebuffer::registerForInterruptType") == 0 && ret == 0 && f != nullptr) {
            irqRegistered(static_cast<UInt32>(reinterpret_cast<UInt64>(b)), *static_cast<void***>(f));
        }
        // Without the accelerator register hooks its setup would program the GPU unchecked: initializeTtl is reported
        // failed, so init stops cleanly and tears TTL down.
        if (accelHeldBack && (ret & 0xFF) != 0 && strcmp(hook.name, "RTHardware::initializeTtl") == 0) { ret = 0; }
        // VCN2's engine (AMDNavi10Hardware +0x3f0) is dropped: VCN is a stub and power-gated on the BC-250. The
        // accelerator's start requires engine 8 (VCN) when capability +0x87 is set and engine 5 when +0x84 is
        // (getHWEngine, hardware vtable +0x310); the capabilities (vtable +0x158) are cleared for the empty slots.
        if ((ret & 0xFF) != 0 && strcmp(hook.name, "Navi10Hardware::allocateHWEngines") == 0) {
            auto& vcn = getMember<OSObject*>(a, kVcnEngineSlot);
            if (vcn != nullptr) {
                vcn->release();
                vcn = nullptr;
            }
            auto* caps = reinterpret_cast<UInt8* (*)(void*)>((*reinterpret_cast<void***>(a))[kHwCapabilities / 8])(a);
            if (caps != nullptr) {
                if (getMember<void*>(a, kVcnEngineSlot) == nullptr) { caps[0x87] = 0; }
                if (getMember<void*>(a, kEngineSlot5) == nullptr) { caps[0x84] = 0; }
            }
        }
        // Without the stop and controller-request guards a late start failure panics: Hardware::init is reported
        // failed instead, so the accelerator does not start.
        if (accelStopUnguarded && (ret & 0xFF) != 0 && strcmp(hook.name, "Hardware::init") == 0) { ret = 0; }
        return ret;
    }

    // One pass-through per hook; integer/pointer arguments only, and no hooked signature has more than six, so the
    // six argument registers are forwarded unchanged.
    template<size_t I>
    UInt64 wrap(void* a, void* b, void* c, void* d, void* e, void* f)
    {
        auto&        hook  = hooks[I];
        const UInt32 flags = wrapBefore(hook, a, b, c);
        if (flags == 0) { return 0; }
        const UInt64 ret = FunctionCast(wrap<I>, hook.org)(a, b, c, d, e, f);
        return wrapAfter(hook, flags, ret, a, b, d, f, __builtin_return_address(0));
    }

    template<size_t... Is>
    struct Seq
    {};
    template<size_t N, size_t... Is>
    struct MakeSeq : MakeSeq<N - 1, N - 1, Is...>
    {};
    template<size_t... Is>
    struct MakeSeq<0, Is...>
    {
        using Type = Seq<Is...>;
    };

    template<size_t... Is>
    mach_vm_address_t wrapperFor(size_t i, Seq<Is...>)
    {
        static const mach_vm_address_t table[] = {reinterpret_cast<mach_vm_address_t>(&wrap<Is>)...};
        return table[i];
    }

    mach_vm_address_t hwlibsStart = 0, hwlibsEnd = 0;

    // Returns the only match of the pattern in the kext, or 0.
    mach_vm_address_t findUnique(const UInt8* pattern, const UInt8* mask, size_t length, mach_vm_address_t slide,
        size_t size)
    {
        size_t first = 0;
        if (!KernelPatcher::findPattern(pattern, mask, length, reinterpret_cast<const void*>(slide), size, &first)) {
            return 0;
        }
        size_t second = first + 1;
        if (second < size && KernelPatcher::findPattern(pattern, mask, length,
                                 reinterpret_cast<const void*>(slide), size, &second)) {
            return 0;
        }
        return slide + first;
    }

    // macOS 26's HWLibs keeps its symbols and also has near-identical functions of other IP versions (GC 10.3,
    // MES/LSDMA register accessors, SDMA 5.2/6.0), so several patterns written for 14.8.9's stripped HWLibs match
    // nothing or more than once there. Symbol first; the pattern's only match otherwise (14.8.9).
    mach_vm_address_t findFunction(KernelPatcher& patcher, size_t id, const char* symbol, const UInt8* pattern,
        size_t length, mach_vm_address_t slide, size_t size)
    {
        if (symbol != nullptr) {
            const auto at = patcher.solveSymbol(id, symbol, slide, size);
            if (at != 0) { return at; }
            patcher.clearError();
        }
        return findUnique(pattern, nullptr, length, slide, size);
    }

    // AMDHardware::unmapDoorbellMemory (14.8.9) calls through the handler at this+0x18 without checking it, and free()
    // calls it even when init failed before storing the handler. Skip it in that case. The guard is installed only if
    // the instruction that loads the handler (mov rdi, [rbx+0x18] at +0x31) is where it is expected.
    constexpr size_t      kUnmapDoorbellHandlerLoad = 0x31;
    constexpr UInt8       kUnmapDoorbellHandlerBytes[] = {0x48, 0x8B, 0x7B, 0x18};
    mach_vm_address_t     orgUnmapDoorbellMemory = 0;

    void wrapUnmapDoorbellMemory(void* self)
    {
        if (getMember<void*>(self, 0x18) == nullptr) {
            BCLOG("BC250HWL", "Hardware::unmapDoorbellMemory(%p): no handler (init failed early), skipped", self);
            return;
        }
        FunctionCast(wrapUnmapDoorbellMemory, orgUnmapDoorbellMemory)(self);
    }

    // Level 23: the accelerator's own register access. AMDHWRegisters::read/write (MMIO at +0x38, or MM_INDEX/DATA
    // above the aperture) carry every accelerator register access; its read-modify-write helpers call them through
    // the vtable. At level 23 every write is logged and not made (reads run and are logged), so the setup's register
    // programming can be checked against Linux (gmc_v10_0, gfxhub/mmhub_v2_0) before it reaches the GPU.
    constexpr UInt32 kAccelLogMax = 3000;
    mach_vm_address_t orgAccelRegRead = 0, orgAccelRegWrite = 0, orgInitVramInfo = 0;
    bool              accelLive = false;
    UInt32            accelWrites = 0, accelReads = 0;

    // Level 26: VM TLB invalidations (GCVM_INVALIDATE_ENG*_* at 0x28A3-0x28D2, the MM hub's at 0x1A6E3-0x1A712) come in
    // the thousands during memory setup and would use up the log; they are counted instead of logged.
    UInt32 accelInvalidateWrites = 0, accelInvalidateReads = 0;
    bool   accelIsInvalidate(UInt32 reg)
    {
        return (reg >= 0x28A3 && reg <= 0x28D2) || (reg >= 0x1A6E3 && reg <= 0x1A712);
    }

    // Level 27 (defined after GVM's FB offset): page-table base writes of the accelerator.
    bool accelIsPdb(UInt32 reg);
    bool accelPdbWrite(void* regs, UInt32 reg, UInt32 value);
    bool accelWritesLive();
    void gcCpBrief(const char* when);

    // Level 27: GC/MMVM_L2_PROTECTION_FAULT_CNTL and _CNTL2. Apple's initializeVmHardware writes 0x1FFC and 0 over
    // the reset 0x3FFFFFFC and 0x20000: CLIENT_ID/OTHER_CLIENT_ID_NO_RETRY_FAULT_INTERRUPT (bits 13-29) cleared makes
    // every client's faults retry faults, retried silently in the UTCL2. Linux (gfxhub/mmhub_v2_0_set_fault_enable_
    // default) writes only the *_ENABLE_DEFAULT bits 2-12 and never CNTL2, so the rest keeps its reset value.
    // bc250noretry=0 keeps Apple's values.
    constexpr UInt32 kGcFaultCntl = 0x1260 + 0x15E8, kMmFaultCntl = 0x1A000 + 0x688, kFaultCntlLinuxBits = 0x1FFC;
    SInt32           accelFaultCntlLinux = -1;    // -1: boot-arg not read yet.

    // 28z10: GC/MMVM_CONTEXT1-15_CNTL. Apple writes 0x3B (enable, depth 1, block size 7) with none of the
    // *_PROTECTION_FAULT_ENABLE_DEFAULT bits, so a faulting request is refused: the GFX CP halted (ME_CNTL
    // 0x15000000) on Firefox's write to an unmapped VA, and with Apple's Navi 10 GPU reset blocked the display stayed
    // green and the machine froze. Linux (gfxhub/mmhub_v2_0_setup_vmid_config) sets the range, dummy page, PDE0,
    // valid, read, write and execute defaults (bits 10-22, even): the access goes to the default page, the fault is
    // still reported, and the GPU keeps running. bc250faultdefault=0 keeps Apple's value.
    constexpr UInt32 kGcContext1Cntl = 0x1260 + 0x1621, kMmContext1Cntl = 0x1A000 + 0x6C1, kContextFaultDefaults = 0x555400;
    SInt32           accelFaultDefaults  = -1;
    UInt32           accelContextCntlLogged = 0;

    bool accelIsContextCntl(UInt32 reg)
    {
        return (reg >= kGcContext1Cntl && reg < kGcContext1Cntl + 15) || (reg >= kMmContext1Cntl && reg < kMmContext1Cntl + 15);
    }

    bool accelContextFaultDefaults()
    {
        if (accelFaultDefaults < 0) {
            UInt32 on = 1;
            PE_parse_boot_argn("bc250faultdefault", &on, sizeof(on));
            accelFaultDefaults = on != 0 ? 1 : 0;
        }
        return accelFaultDefaults == 1;
    }

    bool accelFaultCntlAsLinux()
    {
        if (accelFaultCntlLinux < 0) {
            UInt32 noretry = 1;
            PE_parse_boot_argn("bc250noretry", &noretry, sizeof(noretry));
            accelFaultCntlLinux = noretry != 0 ? 1 : 0;
        }
        return accelFaultCntlLinux == 1;
    }

    // Level 27: the CP went from idle to stuck (fetcher, VM L2 busy) within RTHardware::allocateMemoryResources's first
    // steps: an HDP flush (BIF_BX_PF_HDP_MEM_COHERENCY_FLUSH_CNTL, 0xE17) and a ranged TLB invalidation on GCVM engine
    // 5 (ADDR_RANGE 0x28D1/2, REQ 0x28A8, ACK 0x28BA). The first few of each are logged with the CP's state after them.
    constexpr UInt32 kHdpFlushCntl = 0xD20 + 0xF7, kInvReqFirst = 0x28A3, kInvAckFirst = 0x28B5, kInvRangeFirst = 0x28C7;
    constexpr UInt32 kCpProbeMax = 3;
    UInt32           cpProbeHdp = 0, cpProbeReq = 0, cpProbeAck = 0;

    // Level 27: the accelerator's invalidation request (0x00990001: FLUSH_TYPE 1, L2 PTEs, PDE0 and L1 PTEs only) is
    // never acknowledged on the BC-250 and leaves the GC VM L2 busy for good (the CP's fetcher, then the KIQ, wait
    // behind it). HWLibs' GVM invalidates with 0x02F80000 | VMID mask (FLUSH_TYPE 0, L2 PTEs, PDE0-2, L1 PTEs, bit 25)
    // on ranges too and is acknowledged at once. Requests are sent in GVM's form, followed by Linux's dummy read of the
    // request register (gmc_v10_0_flush_gpu_tlb, GC before 10.3: fast GRBM false ACK) and a wait for the ACK.
    // bc250inv=0 keeps Apple's requests.
    constexpr UInt32 kInvReqGvm = 0x02F80000, kInvAckTimeoutUs = 100000, kInvLogMax = 8;
    constexpr UInt32 kMmInvReqFirst = 0x1A6E3, kMmInvAckFirst = 0x1A6F5, kInvReqToAck = 0x12;
    SInt32           accelInvAsGvm = -1;
    UInt32           accelInvLogged = 0, accelInvTimeouts = 0;

    bool accelIsInvalidateReq(UInt32 reg)
    {
        return (reg >= kInvReqFirst && reg < kInvAckFirst) || (reg >= kMmInvReqFirst && reg < kMmInvAckFirst);
    }

    bool accelInvalidateAsGvm()
    {
        if (accelInvAsGvm < 0) {
            UInt32 inv = 1;
            PE_parse_boot_argn("bc250inv", &inv, sizeof(inv));
            accelInvAsGvm = inv != 0 ? 1 : 0;
            BCLOG("BC250HWL", "level 27: VM invalidation requests %s",
                inv != 0 ? "as GVM's (0x02F80000 | VMIDs)" : "Apple's");
        }
        return accelInvAsGvm == 1;
    }

    void accelInvalidateRequest(void* regs, UInt32 reg, UInt32 value);

    UInt32 wrapAccelRegRead(void* regs, UInt32 reg)
    {
        const auto value = FunctionCast(wrapAccelRegRead, orgAccelRegRead)(regs, reg);
        if (accelIsInvalidate(reg)) {
            accelInvalidateReads++;
            if (accelLive && reg >= kInvAckFirst && reg < kInvRangeFirst && value != 0 &&
                cpProbeAck < kCpProbeMax) {
                cpProbeAck++;
                BCLOG("BC250HWL", "Accel: VM invalidation ACK 0x%05X -> 0x%08X after %u ack reads", reg, value,
                    accelInvalidateReads);
                gcCpBrief("after invalidation ack");
            }
            return value;
        }
        if (++accelReads <= kAccelLogMax) { BCLOG("BC250HWL", "Accel: R 0x%05X -> 0x%08X", reg, value); }
        return value;
    }

    void wrapAccelRegWrite(void* regs, UInt32 reg, UInt32 value)
    {
        if (accelIsInvalidate(reg)) {
            if ((++accelInvalidateWrites & 0x3FF) == 1) {
                BCLOG("BC250HWL", "Accel%s: VM invalidation W 0x%05X = 0x%08X (%u invalidation writes, %u ack reads so far)",
                    accelLive ? "" : " dry", reg, value, accelInvalidateWrites, accelInvalidateReads);
            }
            if (accelLive && accelInvalidateWrites <= 12) {
                cpProbeReq++;    // Counted for the log only.
                BCLOG("BC250HWL", "Accel: VM invalidation W 0x%05X = 0x%08X (%s)", reg, value,
                    reg < kInvAckFirst ? "REQ" : reg < kInvRangeFirst ? "ACK" : "ADDR_RANGE");
            }
            if (accelLive && accelIsInvalidateReq(reg) && accelInvalidateAsGvm()) {
                accelInvalidateRequest(regs, reg, value);
            } else if (accelLive) {
                FunctionCast(wrapAccelRegWrite, orgAccelRegWrite)(regs, reg, value);
            }
            if (accelLive && reg >= kInvReqFirst && reg < kInvAckFirst &&
                accelInvalidateWrites <= 12) {
                gcCpBrief("after invalidation REQ");
            }
            return;
        }
        if (++accelWrites <= kAccelLogMax) {
            BCLOG("BC250HWL", "Accel%s: W 0x%05X = 0x%08X", accelLive ? "" : " dry", reg, value);
        }
        if (accelWritesLive()) {
            // GCMC_VM_MX_L1_TLB_CNTL: the first write of initializeVmHardware (powerUp); the CP was found stuck
            // before the KIQ, so its state is taken here and after the VM setup's fault controls.
            if (reg == 0x1260 + 0x1727) { gcCpBrief("before initializeVmHardware"); }
            if ((reg == kGcFaultCntl || reg == kMmFaultCntl || reg == kGcFaultCntl + 1 || reg == kMmFaultCntl + 1) &&
                accelFaultCntlAsLinux()) {
                const auto current = FunctionCast(wrapAccelRegRead, orgAccelRegRead)(regs, reg);
                const bool cntl2   = reg == kGcFaultCntl + 1 || reg == kMmFaultCntl + 1;
                const auto asLinux = cntl2 ? current : (current & ~kFaultCntlLinuxBits) | (value & kFaultCntlLinuxBits);
                BCLOG("BC250HWL", "Accel: W 0x%05X = 0x%08X, Apple's 0x%08X with the rest as Linux (reset 0x%08X)", reg,
                    asLinux, value, current);
                FunctionCast(wrapAccelRegWrite, orgAccelRegWrite)(regs, reg, asLinux);
                if (reg == kMmFaultCntl + 1) { gcCpBrief("after VM fault controls"); }
                return;
            }
        }
        if (accelWritesLive() && accelIsContextCntl(reg) && (value & 1) != 0 &&
            accelContextFaultDefaults())
        {
            const UInt32 withDefaults = value | kContextFaultDefaults;
            if (++accelContextCntlLogged <= 32) {
                BCLOG("BC250HWL", "Accel: W 0x%05X = 0x%08X, Apple's 0x%08X with Linux's fault defaults", reg,
                    withDefaults, value);
            }
            FunctionCast(wrapAccelRegWrite, orgAccelRegWrite)(regs, reg, withDefaults);
            return;
        }
        if (accelWritesLive() && accelIsPdb(reg) && accelPdbWrite(regs, reg, value)) { return; }
        if (accelWritesLive()) { FunctionCast(wrapAccelRegWrite, orgAccelRegWrite)(regs, reg, value); }
        if (accelWritesLive() && reg == kHdpFlushCntl && cpProbeHdp < kCpProbeMax) {
            cpProbeHdp++;
            IODelay(100);
            gcCpBrief("after HDP flush");
        }
    }

    void accelInvalidateRequest(void* regs, UInt32 reg, UInt32 value)
    {
        auto       read  = [regs](UInt32 r) { return FunctionCast(wrapAccelRegRead, orgAccelRegRead)(regs, r); };
        const auto vmids = value & 0xFFFF;
        const auto req   = kInvReqGvm | vmids;
        FunctionCast(wrapAccelRegWrite, orgAccelRegWrite)(regs, reg, req);
        read(reg);    // Linux's dummy read (GC before 10.3).
        // After a few timeouts the wait is dropped (each would cost 100 ms, and there are about a thousand requests).
        UInt32 us = 0, ack = 0;
        if (accelInvTimeouts < 4) {
            for (; us < kInvAckTimeoutUs; us++) {
                ack = read(reg + kInvReqToAck);
                if ((ack & vmids) == vmids) { break; }
                IODelay(1);
            }
            if (us >= kInvAckTimeoutUs) { accelInvTimeouts++; }
        }
        if (accelInvLogged < kInvLogMax || us >= kInvAckTimeoutUs) {
            accelInvLogged++;
            BCLOG("BC250HWL", "Accel: VM invalidation REQ 0x%05X = 0x%08X (Apple's 0x%08X), ACK 0x%08X after %u us%s", reg,
                req, value, ack, us, us >= kInvAckTimeoutUs ? " (timeout)" : "");
        }
    }

    void raiseVisibleVram(void* memory);

    // AMDHWMemory::initVRAMInfo: the VRAM layout the accelerator gets from its hardware interface (+0x40 size, +0x48
    // CPU-visible size, +0x50 MC base, +0x58; +0x60 = +0x50 - +0x58, the offset adjustVRAMAddress removes).
    bool wrapInitVramInfo(void* memory)
    {
        const bool ret = FunctionCast(wrapInitVramInfo, orgInitVramInfo)(memory);
        BCLOG("BC250HWL", "HWMemory::initVRAMInfo <<< %d: +0x40 0x%llX, +0x48 0x%llX, +0x50 0x%llX, +0x58 0x%llX, "
                          "+0x60 0x%llX",
            ret, getMember<UInt64>(memory, 0x40), getMember<UInt64>(memory, 0x48), getMember<UInt64>(memory, 0x50),
            getMember<UInt64>(memory, 0x58), getMember<UInt64>(memory, 0x60));
        if (ret) { raiseVisibleVram(memory); }
        return ret;
    }

    void hookAccelRegisters(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        KernelPatcher::RouteRequest requests[] = {
            {"__ZN29AMDRadeonX6000_AMDHWRegisters4readEj", wrapAccelRegRead, orgAccelRegRead},
            {"__ZN29AMDRadeonX6000_AMDHWRegisters5writeEjj", wrapAccelRegWrite, orgAccelRegWrite},
            {"__ZN26AMDRadeonX6000_AMDHWMemory12initVRAMInfoEv", wrapInitVramInfo, orgInitVramInfo},
        };
        if (patcher.routeMultiple(id, requests, arrsize(requests), slide, size)) {
            BCLOG("BC250HWL", "level 23: accelerator registers hooked (writes %s)", accelLive ? "made" : "logged, not made");
        }
        else {
            // Without the write hook the setup would program the GPU unchecked: hold it back as at level 22.
            BCLOG("BC250HWL", "level 23: accelerator registers not hooked; its setup does not run");
            patcher.clearError();
            accelHeldBack = true;
        }
    }

    // Level 25: AMDGraphicsAccelerator::start's failure path (after its event log exists) calls stop, which releases
    // the event log at +0x1ea0 and clears it; start's common exit then reads +0x1ea0 again and panics on the null
    // (first level 25 run). When stop is called from inside start, the event log is retained first and put back
    // after stop, so the exit finds it alive (one event log leaks). The state start's success depends on is logged:
    // hardware interface +0x1a38 (isDeviceValid: +0x30d, +0x30e; powerUp sets +0x30c/+0x30f), +0xdc8, +0x1f40.
    constexpr UInt32 kAccelEventLog = 0x1EA0, kAccelHw = 0x1A38;
    mach_vm_address_t orgAccelStop = 0;

    void wrapAccelStop(void* accel, void* provider)
    {
        if (!accelInStart) {
            FunctionCast(wrapAccelStop, orgAccelStop)(accel, provider);
            return;
        }
        auto* hw = getMember<UInt8*>(accel, kAccelHw);
        BCLOG("BC250HWL", "Accelerator::start failed: stop from start; hw %p flags 30c-30f %u %u %u %u, +0xdc8 0x%X, "
                          "+0x1f40 %p",
            hw, hw ? hw[0x30C] : 0, hw ? hw[0x30D] : 0, hw ? hw[0x30E] : 0, hw ? hw[0x30F] : 0,
            getMember<UInt32>(accel, 0xDC8), getMember<void*>(accel, 0x1F40));
        if (hw != nullptr) {
            // AMDHardware::isDeviceValid (vtable +0x2a0), start's first success condition.
            const auto isDeviceValid = reinterpret_cast<bool (*)(void*)>((*reinterpret_cast<void***>(hw))[0x2A0 / 8]);
            BCLOG("BC250HWL", "Accelerator::start failed: isDeviceValid now %d, +0x19f8 %p, +0x1fa8 %p",
                isDeviceValid(hw), getMember<void*>(accel, 0x19F8), getMember<void*>(accel, 0x1FA8));
        }
        auto* log = getMember<OSObject*>(accel, kAccelEventLog);
        if (log != nullptr) { log->retain(); }
        FunctionCast(wrapAccelStop, orgAccelStop)(accel, provider);
        getMember<OSObject*>(accel, kAccelEventLog) = log;
        BCLOG("BC250HWL", "Accelerator::start failed: event log %p kept for start's exit (Apple reads it after stop)",
            log);
    }

    // Level 25: the accelerator's stop (from start's failure path) sends the framebuffer controller request 0xB
    // (AmdRadeonController::callPlatformFunctionFromDrvr), which releases the controller's services object (+0x7928,
    // made by createControllerServices); the controller keeps running the display and its powerUp (WindowServer
    // opening the framebuffer) then calls through the null (second level 25 run). While the accelerator is failing out
    // of start, request 0xB is not passed on, leaving the controller as when the accelerator fails earlier (levels
    // 22-24). Every request from the accelerator is logged.
    constexpr UInt32 kControllerReleaseServices = 0xB;
    mach_vm_address_t orgControllerDrvrFunction = 0;

    IOReturn wrapControllerDrvrFunction(void* controller, UInt32 function, void* p1, void* p2, void* p3)
    {
        if (function == kControllerReleaseServices && accelInStart) {
            BCLOG("BC250HWL", "Controller request 0x%X from the failing accelerator start not passed on (keeps the "
                              "controller's services for the display)",
                function);
            return kIOReturnSuccess;
        }
        return FunctionCast(wrapControllerDrvrFunction, orgControllerDrvrFunction)(controller, function, p1, p2, p3);
    }

    // Level 28: GPU statistics for the usual monitoring tools (Activity Monitor, iStat, HWMonitor), which read the
    // accelerator's "PerformanceStatistics". AMDHardware::publishPMStatistics(dict, bool) copies PowerPlay's values
    // from hardware +0x20580 (core clock, MHz x 100), +0x20584 (memory clock), +0x20588 (activity %), +0x2058C
    // (temperature, m°C), +0x20590 (power, W x 100), +0x20594/+0x20598 (fan RPM/%); with PowerPlay blocked they stay 0.
    // It is replaced (no original called, so no trampoline space) by the same keys filled from the SMU telemetry:
    // GFX clock, Tctl (the GPU shares the die with the CPU) and the GRBM_STATUS busy share; the others as Apple's
    // fields hold them. bc250stats=0 leaves Apple's function in place.
    constexpr UInt32 kHwPmCoreClock = 0x20580, kHwPmMemClock = 0x20584, kHwPmActivity = 0x20588, kHwPmTemp = 0x2058C,
                     kHwPmPower = 0x20590, kHwPmFanRpm = 0x20594, kHwPmFanPercent = 0x20598;

    void wrapPublishPMStatistics(void* hw, OSDictionary* dict, bool)
    {
        if (hw == nullptr || dict == nullptr) { return; }
        const auto& t   = BC250Smu::singleton().telemetry();
        auto        put = [dict](const char* key, UInt64 value) {
            if (auto* number = OSNumber::withNumber(value, 64)) {
                dict->setObject(key, number);
                number->release();
            }
        };
        const bool smu = t.valid;
        put("Core Clock(MHz)", smu ? t.gfxClockMHz : getMember<UInt32>(hw, kHwPmCoreClock) / 100);
        put("Memory Clock(MHz)", getMember<UInt32>(hw, kHwPmMemClock) / 100);
        put("GPU Activity(%)", t.gpuBusyValid ? t.gpuBusyPercent : getMember<UInt32>(hw, kHwPmActivity));
        put("Temperature(C)", smu && t.tctlDeciC > 0 ? static_cast<UInt64>(t.tctlDeciC / 10) :
                                                      getMember<UInt32>(hw, kHwPmTemp) / 1000);
        put("Total Power(W)", getMember<UInt32>(hw, kHwPmPower) / 100);
        put("Fan Speed(RPM)", getMember<UInt32>(hw, kHwPmFanRpm));
        put("Fan Speed(%)", getMember<UInt32>(hw, kHwPmFanPercent));
    }

    void guardUnmapDoorbellMemory(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        const auto fn = patcher.solveSymbol(id, "__ZN26AMDRadeonX6000_AMDHardware19unmapDoorbellMemoryEv", slide, size);
        if (fn == 0 || memcmp(reinterpret_cast<const void*>(fn + kUnmapDoorbellHandlerLoad), kUnmapDoorbellHandlerBytes,
                           sizeof(kUnmapDoorbellHandlerBytes)) != 0)
        {
            BCLOG("BC250HWL", "unmapDoorbellMemory guard not installed (function not as on 14.8.9)");
            patcher.clearError();
            return;
        }
        KernelPatcher::RouteRequest request {nullptr, wrapUnmapDoorbellMemory, orgUnmapDoorbellMemory};
        request.from = fn;
        if (patcher.routeMultiple(id, &request, 1)) {
            BCLOG("BC250HWL", "unmapDoorbellMemory guard installed");
        }
        else {
            BCLOG("BC250HWL", "unmapDoorbellMemory guard failed to route");
            patcher.clearError();
        }
    }

    // TTL's bgm_create requires the IP-discovery binary_header.version_major (at header+4) to be 1, but the BC-250's
    // PSP publishes a valid table (signature 0x28211407) whose version_major is 0 (as Linux reads it too; Linux checks
    // only the signature). The check is "cmpw $0x1, 0x4(%r15)" right before the signature compare (81 FB 07 14 21 28).
    // Change its immediate from 1 to 0 so the BC-250's version passes; the parse path is otherwise the same.
    const UInt8 kDiscVersionPattern[]     = {0x66, 0x41, 0x83, 0x7F, 0x04, 0x01, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
        0x81, 0xFB, 0x07, 0x14, 0x21, 0x28};
    const UInt8 kDiscVersionPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    constexpr size_t kDiscVersionImmOffset = 5;

    void patchDiscoveryVersion(mach_vm_address_t slide, size_t size)
    {
        const auto at =
            findUnique(kDiscVersionPattern, kDiscVersionPatternMask, sizeof(kDiscVersionPattern), slide, size);
        if (at == 0) {
            BCLOG("BC250HWL", "discovery version check: pattern not found once; not patched");
            return;
        }
        auto* imm = reinterpret_cast<UInt8*>(at + kDiscVersionImmOffset);
        if (*imm != 0x01) {
            BCLOG("BC250HWL", "discovery version check: immediate is 0x%X, not 1; left alone", *imm);
            return;
        }
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            BCLOG("BC250HWL", "discovery version check: cannot enable kernel writing");
            return;
        }
        *imm = 0x00;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        BCLOG("BC250HWL", "discovery version check: bgm_create now accepts version_major 0 (patched at +0x%llX)",
            at - slide);
    }

    // Level 7: bgm_create's IP version gates. After parsing the discovery table, bgm_create looks up NBIF (0xc00c0203),
    // PCIE (0204), MP0 (0205), OSSSYS (0206) and SMUIO (0207) by CAIL block type, and each handler accepts only listed
    // versions. The BC-250's NBIF 2.1.1, PCIE 4.2.0 and SMUIO 11.0.8 are not listed. Linux drives NBIO 2.1.1 as 2.3.0
    // (nbio_v2_3) and SMUIO 11.0.8 as 11.0.0 (smuio_v11_0); PCIE has no Linux counterpart and 4.1.0 is used because
    // 4.0.0's init does an extra indirect SMN read. MP0 and MP1 are left alone: Linux gives 11.0.8 its own PSP and
    // SMU code, and an unmatched version keeps Apple's away from them. The stages that follow (0209-020b) only read
    // CAIL properties and the PCIe link registers in config space (checked in X6000HWLibs 14.8.9).
    //
    // From level 10 GVM's sw_init runs, and its four stages look up UMC, GC, HDP and ATHUB in exact-match version
    // tables (major.minor.rev) that list Navi 1x versions but none of the BC-250's. Linux drives GC 10.1.3 like
    // 10.1.10 (Navi 10), HDP 5.0.1 like 5.0.0, and MMHUB/ATHUB 2.0.3 like 2.0.0; UMC 8.1.1 has no Linux counterpart
    // for this family, and 8.0.0 is the nearest version Apple's table lists (a guess).
    struct IpRemap
    {
        UInt32 type, major, minor, rev;
        UInt32 newMajor, newMinor, newRev;
        const char* name;
    };
    constexpr IpRemap kIpRemaps[] = {
        {0x42, 2, 1, 1, 2, 3, 0, "NBIF"},
        {0x3D, 4, 2, 0, 4, 1, 0, "PCIE"},
        {0x07, 11, 0, 8, 11, 0, 0, "SMUIO"},
        {0x46, 8, 1, 1, 8, 0, 0, "UMC"},
        {0x0B, 10, 1, 3, 10, 1, 10, "GC"},
        {0x22, 5, 0, 1, 5, 0, 0, "HDP"},
        {0x1B, 2, 0, 3, 2, 0, 0, "MMHUB"},
        {0x1C, 2, 0, 3, 2, 0, 0, "ATHUB"},
    };

    // The parsed topology: entry count at +0x1C, then 0x260-byte entries from +0x20 of {UInt32 type; ...; UInt32 major
    // at +0x8, minor at +0xC, rev at +0x10}.
    constexpr size_t kTopologyCount     = 0x1C;
    constexpr size_t kTopologyEntries   = 0x20;
    constexpr size_t kTopologyEntrySize = 0x260;
    constexpr UInt32 kTopologyMaxCount  = 64;

    // bgm_create's prologue (unique in X6000HWLibs 14.8.9); checked by finding "mov esi, 0xc00c0203" shortly after.
    const UInt8 kBgmCreatePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
        0x48, 0x83, 0xEC, 0x28, 0x31, 0xC0, 0x48, 0x89, 0x45, 0xD0, 0x48, 0x89, 0x45, 0xC0, 0x88, 0x45, 0xCD, 0x88,
        0x45, 0xCE, 0x48, 0x89, 0x45, 0xB8, 0x88, 0x45, 0xCF, 0x41, 0xBE, 0x01, 0x00, 0x00, 0x00};
    const UInt8 kBgmCreateError203[] = {0xBE, 0x03, 0x02, 0x0C, 0xC0};
    constexpr size_t kBgmCreateSpan = 0x300;

    // Its only call to the discovery parser, followed by the 0xc00c0202 error it raises:
    // mov rsi, r12; mov rdx, r13; call parse; mov r14d, eax; mov rdi, [r13]; mov rax, [rbp-0x30]; mov [rax+0x28], rdi;
    // test r14d, r14d; je +x; mov rdi, [rax+0x20]; mov esi, 0xc00c0202
    const UInt8 kParseCallPattern[] = {0x4C, 0x89, 0xE6, 0x4C, 0x89, 0xEA, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x41, 0x89,
        0xC6, 0x49, 0x8B, 0x7D, 0x00, 0x48, 0x8B, 0x45, 0xD0, 0x48, 0x89, 0x78, 0x28, 0x45, 0x85, 0xF6, 0x74, 0x00,
        0x48, 0x8B, 0x78, 0x20, 0xBE, 0x02, 0x02, 0x0C, 0xC0};
    const UInt8 kParseCallPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    constexpr size_t kParseCallRel = 7;
    static_assert(sizeof(kParseCallPattern) == sizeof(kParseCallPatternMask));

    // Returned by bgm_create at level 7 after it succeeded, so TTL stops through its "bgm_create was unsuccessful"
    // path before the first BGM operation (init/power-up), which has not been reviewed. The context is leaked, not
    // destroyed; the caller only copies it out on success.
    constexpr UInt32 kBgmCreateStopped = 0xBC250007;

    mach_vm_address_t orgBgmCreate = 0, orgDiscoveryParse = 0;
    // Set once level 8's stop (IpiInitializeIpInterfaces blocked, VRAM write-back skipped) is installed; bgm_create's
    // success is only passed on then, so a missing hook falls back to the level 7 stop.
    bool tlsStopInstalled = false;

    void remapTopology(UInt8* topology)
    {
        const auto count = getMember<UInt32>(topology, kTopologyCount);
        if (count == 0 || count > kTopologyMaxCount) {
            BCLOG("BC250HWL", "topology: %u entries, not as expected; left alone", count);
            return;
        }
        for (UInt32 i = 0; i < count; i++) {
            auto* entry = topology + kTopologyEntries + i * kTopologyEntrySize;
            auto& type  = getMember<UInt32>(entry, 0x0);
            auto& major = getMember<UInt32>(entry, 0x8);
            auto& minor = getMember<UInt32>(entry, 0xC);
            auto& rev   = getMember<UInt32>(entry, 0x10);
            BCLOG("BC250HWL", "topology[%u]: type 0x%X version %u.%u.%u", i, type, major, minor, rev);
            for (const auto& remap : kIpRemaps) {
                if (type != remap.type) { continue; }
                if (major != remap.major || minor != remap.minor || rev != remap.rev) {
                    BCLOG("BC250HWL", "topology: %s is %u.%u.%u, not %u.%u.%u; left alone", remap.name, major, minor,
                        rev, remap.major, remap.minor, remap.rev);
                    continue;
                }
                major = remap.newMajor;
                minor = remap.newMinor;
                rev   = remap.newRev;
                BCLOG("BC250HWL", "topology: %s now reported as %u.%u.%u", remap.name, major, minor, rev);
            }
        }
    }

    UInt32 wrapDiscoveryParse(void* ctx, void* input, UInt8** topology, void* d, void* e)
    {
        const auto ret = FunctionCast(wrapDiscoveryParse, orgDiscoveryParse)(ctx, input, topology, d, e);
        BCLOG("BC250HWL", "discovery parse -> 0x%X, topology %p", ret, topology ? *topology : nullptr);
        if (ret == 0 && topology != nullptr && *topology != nullptr) { remapTopology(*topology); }
        return ret;
    }

    UInt32 wrapBgmCreate(void* input, void* output)
    {
        BCLOG("BC250HWL", "bgm_create(%p, %p) >>>", input, output);
        const auto ret = FunctionCast(wrapBgmCreate, orgBgmCreate)(input, output);
        if (ret != 0) {
            BCLOG("BC250HWL", "bgm_create <<< 0x%X", ret);
            return ret;
        }
        if (tlsStopInstalled) {
            BCLOG("BC250HWL", "bgm_create <<< 0 (succeeded)");
            return ret;
        }
        BCLOG("BC250HWL", "bgm_create <<< 0 (succeeded); level 7 stops here, returning 0x%X", kBgmCreateStopped);
        return kBgmCreateStopped;
    }

    // Level 8: TlsSwInit runs IpiValidateTopology (ipi_bgm_create, then the SWIP boot sequence checked against the
    // topology), then IpiInitializeIpInterfaces (one interface per SWIP: GC, SDMA, PSP, SMU, ...). Past bgm_create,
    // ipi_bgm_create and IpiValidateTopology only query the BGM (RCC_CONFIG_MEMSIZE, PCIe link registers in config
    // space) and fill TTL's own state. Level 8 lets IpiValidateTopology run and blocks IpiInitializeIpInterfaces, so
    // TlsSwInit fails ("Failed to create TLS") and tears the BGM down. That teardown would write a saved 64 KB buffer
    // back to VRAM offset 0 (where Navi 10 keeps a VBIOS copy; on the BC-250 it is the desktop framebuffer), so the
    // write-back is skipped. All three are stripped; found by their prologues (unique in X6000HWLibs 14.8.9).
    const UInt8 kIpiValidateTopologyPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
        0x83, 0xEC, 0x10, 0x49, 0x89, 0xF6, 0x48, 0x89, 0xFB, 0x48, 0xC7, 0x45, 0xD8, 0x00, 0x00, 0x00, 0x00};
    const UInt8 kIpiInitIpInterfacesPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
        0x53, 0x48, 0x83, 0xEC, 0x18, 0x85, 0xF6, 0x74, 0x60, 0x49, 0x89, 0xD7, 0x48, 0x89, 0x7D, 0xD0};
    const UInt8 kVramRestorePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x18,
        0x49, 0x89, 0xFE, 0x31, 0xDB, 0x4C, 0x8D, 0x7D, 0xD8, 0x49, 0x89, 0x1F, 0x4C, 0x8D, 0x45, 0xE0, 0x49, 0x89,
        0x18, 0x48, 0x8B, 0x3F, 0xBA, 0x00, 0x00, 0x01, 0x00, 0x31, 0xF6, 0x4C, 0x89, 0xF9};

    mach_vm_address_t orgIpiValidateTopology = 0, orgIpiInitIpInterfaces = 0, orgVramRestore = 0;

    // TTL reports its errors ("NULL GVM sw_init entry!", ...) through a log callback in its services struct:
    // services = entry 1 of the root table (root = *ipi, table = *root, services = *(table + 0x28), as the accessor
    // 0xc39d704 reads it), called as log(services->cookie at +0x8, function, file, line, message). On this setup those
    // messages reach no log. From level 8 the callback is wrapped so every TTL message is copied to debug.bc250.log
    // before the original runs; nothing else changes.
    constexpr size_t kTtlServicesEntry = 0x28, kTtlServicesLog = 0x70;
    constexpr UInt32 kTtlLogMax        = 300;

    using TtlLogFn = void (*)(void*, const char*, const char*, UInt32, const char*);
    TtlLogFn orgTtlLog   = nullptr;
    UInt32   ttlLogLines = 0;

    // TTL passes its own string constants; anything else is not dereferenced.
    const char* hwlibsString(const char* s)
    {
        const auto a = reinterpret_cast<mach_vm_address_t>(s);
        return a >= hwlibsStart && a < hwlibsEnd ? s : "?";
    }

    void wrapTtlLog(void* cookie, const char* function, const char* file, UInt32 line, const char* message)
    {
        if (++ttlLogLines <= kTtlLogMax) {
            BCLOG("TTL", "%s:%u %s", hwlibsString(function), line, hwlibsString(message));
        }
        if (orgTtlLog != nullptr) { orgTtlLog(cookie, function, file, line, message); }
    }

    void captureTtlLog(void* ipi)
    {
        if (orgTtlLog != nullptr || ipi == nullptr) { return; }
        auto* root = *static_cast<UInt8**>(ipi);
        if (root == nullptr) { return; }
        auto* table = *reinterpret_cast<UInt8**>(root);
        if (table == nullptr) { return; }
        auto* services = getMember<UInt8*>(table, kTtlServicesEntry);
        if (services == nullptr) { return; }
        auto& slot = getMember<TtlLogFn>(services, kTtlServicesLog);
        const auto current = reinterpret_cast<mach_vm_address_t>(slot);
        if (current == 0 || slot == wrapTtlLog) {
            BCLOG("BC250HWL", "TTL log: callback slot is %p; not captured", reinterpret_cast<void*>(slot));
            return;
        }
        // The services struct may be in read-only data, so the slot is written like the other patches here.
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            BCLOG("BC250HWL", "TTL log: cannot enable kernel writing; not captured");
            return;
        }
        orgTtlLog = slot;
        slot      = wrapTtlLog;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        BCLOG("BC250HWL", "TTL log: callback %p (services %p) captured into debug.bc250.log",
            reinterpret_cast<void*>(orgTtlLog), services);
    }

    UInt64 wrapIpiValidateTopology(void* tls, void* b)
    {
        captureTtlLog(tls);
        BCLOG("BC250HWL", "IpiValidateTopology(%p, %p) >>>", tls, b);
        const auto ret = FunctionCast(wrapIpiValidateTopology, orgIpiValidateTopology)(tls, b);
        BCLOG("BC250HWL", "IpiValidateTopology <<< 0x%llX (%s)", ret, (ret & 0xFF) != 0 ? "passed" : "failed");
        return ret;
    }

    UInt64 wrapIpiInitIpInterfaces(void* tls, UInt32 count, void* list)
    {
        const auto ret = FunctionCast(wrapIpiInitIpInterfaces, orgIpiInitIpInterfaces)(tls, count, list);
        BCLOG("BC250HWL", "IpiInitializeIpInterfaces(%u SWIPs) <<< 0x%llX", count, ret);
        return ret;
    }

    // Level 9: IpiInitializeIpInterfaces only allocates one interface per SWIP and records its function table. The
    // next step, TlsExecuteIpEntrySeq(tls, event, ...), walks the SWIPs in boot order and calls each one's
    // notify_event (event 0 = sw_init: BGM, GVM, PSP, SMU, GC, SDMA, MES, VCN, JPEG, DMCU on Navi 10), which is where
    // SWIP code first runs. Level 9 blocks it and logs the sequence. Its TLS object holds the SWIP count at +0x10 and
    // the ids from +0x14.
    const UInt8 kTlsExecuteIpEntrySeqPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
        0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x41, 0x89, 0xF7, 0x49, 0x89, 0xFE, 0x83, 0xFE, 0x07};
    constexpr size_t kTlsSwipCount = 0x10;
    constexpr size_t kTlsSwipIds   = 0x14;

    mach_vm_address_t orgTlsExecuteIpEntrySeq = 0;

    UInt64 wrapTlsExecuteIpEntrySeq(UInt8* tls, UInt32 event, void* c)
    {
        const auto count = getMember<UInt32>(tls, kTlsSwipCount);
        char       ids[96] = {};
        size_t     used    = 0;
        for (UInt32 i = 0; i < count && i < 32 && used + 5 < sizeof(ids); i++) {
            used += snprintf(ids + used, sizeof(ids) - used, "%s%u", i ? "," : "",
                getMember<UInt32>(tls, kTlsSwipIds + i * sizeof(UInt32)));
        }
        BCLOG("BC250HWL", "TlsExecuteIpEntrySeq(event %u, %u SWIPs: %s) >>>", event, count, ids);
        const auto ret = FunctionCast(wrapTlsExecuteIpEntrySeq, orgTlsExecuteIpEntrySeq)(tls, event, c);
        BCLOG("BC250HWL", "TlsExecuteIpEntrySeq(event %u) <<< 0x%llX", event, ret);
        return ret;
    }

    // Level 10: SWIPs are let through one at a time. TlsExecuteIpEntrySeq sends each SWIP an event through
    // 0xc3a65b8(ipi, swip, event, context) (SWIP id x event 0-8, to each ipi_*_notify_event); the events are
    // 0 = sw_init, 1 = hw_init/power-up, 2 = sw_fini, 3 = hw_fini/power-down. Event 0 runs only for the first
    // bc250swip=N (default 1) SWIPs of Navi 10's boot order; the next one is refused, which fails the sequence as a
    // SWIP init failure would. Event 2 (the failure path's teardown) runs only for SWIPs whose event 0 ran. Every
    // other event is refused. A refused event returns false without running any SWIP code, as at level 9.
    const UInt8 kIpiSwipEventPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x45, 0x31,
        0xFF, 0xFF, 0xCE, 0x83, 0xFE, 0x0F, 0x0F, 0x87};
    constexpr UInt32      kSwipBootOrder[] = {3, 5, 6, 8, 7, 2, 14, 9, 15, 10};
    constexpr const char* kSwipNames[]     = {"?", "ISP", "SDMA", "BGM", "?", "GVM", "PSP", "GC", "SMU", "VCN", "DMCU",
            "?", "?", "?", "MES", "JPEG", "LSDMA"};
    constexpr UInt32      kSwipEventSwInit = 0, kSwipEventSwFini = 2;

    mach_vm_address_t orgIpiSwipEvent = 0;
    UInt32            swipAllowed     = 1;
    UInt32            swipInitialised = 0;    // Bit per SWIP id whose sw_init ran.

    const char* swipName(UInt32 swip) { return swip < arrsize(kSwipNames) ? kSwipNames[swip] : "?"; }

    // Address ranges of the four AMD kexts, to name the kext a callback lives in.
    struct KextRange
    {
        const char*       name;
        mach_vm_address_t start, end;
    };
    KextRange kextRanges[] = {{"X6000FB", 0, 0}, {"HWServices", 0, 0}, {"HWLibs", 0, 0}, {"X6000", 0, 0}};

    const char* kextOf(mach_vm_address_t a, mach_vm_address_t* offset)
    {
        for (const auto& r : kextRanges) {
            if (r.start != 0 && a >= r.start && a < r.end) {
                *offset = a - r.start;
                return r.name;
            }
        }
        *offset = a;
        return "outside the AMD kexts";
    }

    // SWIP component provider. Each SWIP's create function takes its four entry points (sw_init first) from a
    // default table, unless 0xc3a326e(id, out) finds a provider answer: it calls a global provider(ctx, 1, &id, &answer)
    // (both globals set at runtime through 0xc3a325a) and, if the answer's two flags are set, copies its four entry
    // points to out. GVM's sw_init entry arrives NULL this way. From level 10 every lookup is logged, together with the
    // provider's location. The globals are read from the lookup's own "mov r8, [rip+x]" (+0xB) and "mov rax, [rip+x]"
    // (+0x1A).
    const UInt8 kSwipProviderLookupPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x4C,
        0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x45, 0x31, 0xF6, 0x4D, 0x85, 0xC0, 0x74, 0x00, 0x48, 0x8B, 0x05, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74};
    const UInt8 kSwipProviderLookupMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    static_assert(sizeof(kSwipProviderLookupPattern) == sizeof(kSwipProviderLookupMask));
    constexpr size_t kProviderFnLoad = 0xB, kProviderCtxLoad = 0x1A, kRipLoadLength = 7, kRipLoadDisp = 3;

    mach_vm_address_t orgSwipProviderLookup = 0;
    void**            swipProviderFn        = nullptr;
    void**            swipProviderCtx       = nullptr;

    // GVM's sw_init (gvm.c) runs four stages that each look up one block in the SWIP's discovery input and its
    // version in an exact-match table: UMC (0x46), GC (0x0B), HDP (0x22, instance 1 checked), ATHUB (0x1C). A stage
    // returns 0x14 if the block is missing and 1 if its version is not listed. From level 10 each return is logged.
    struct GvmStage
    {
        const char*       name;
        const UInt8*      pattern;
        size_t            length;
        mach_vm_address_t org;
    };
    const UInt8 kGvmUmcPattern[]   = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
          0x83, 0xEC, 0x18, 0x49, 0x89, 0xF7, 0x48, 0x89, 0xFB, 0xB8, 0x14, 0x00, 0x00, 0x00, 0x41, 0x83, 0x7C, 0x07,
          0xF0, 0x46};
    const UInt8 kGvmGcPattern[]    = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
           0x83, 0xEC, 0x18, 0x49, 0x89, 0xF7, 0x48, 0x89, 0xFB, 0xB8, 0x14, 0x00, 0x00, 0x00, 0x41, 0x83, 0x7C, 0x07,
           0xF0, 0x0B};
    const UInt8 kGvmHdpPattern[]   = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
          0x83, 0xEC, 0x18, 0x49, 0x89, 0xF7, 0x45};
    const UInt8 kGvmAthubPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
        0x48, 0x83, 0xEC, 0x18, 0x49, 0x89, 0xF7, 0x48, 0x89, 0xFB, 0xB8, 0x14, 0x00, 0x00, 0x00, 0x41, 0x83, 0x7C,
        0x07, 0xF0, 0x1C};
    GvmStage gvmStages[] = {
        {"UMC", kGvmUmcPattern, sizeof(kGvmUmcPattern), 0},
        {"GC", kGvmGcPattern, sizeof(kGvmGcPattern), 0},
        {"HDP", kGvmHdpPattern, sizeof(kGvmHdpPattern), 0},
        {"ATHUB", kGvmAthubPattern, sizeof(kGvmAthubPattern), 0},
    };

    template<size_t I>
    UInt32 wrapGvmStage(void* gvm, void* input, void* output)
    {
        const auto ret = FunctionCast(wrapGvmStage<I>, gvmStages[I].org)(gvm, input, output);
        BCLOG("BC250HWL", "GVM sw_init stage %u (%s) <<< 0x%X%s", static_cast<UInt32>(I + 1), gvmStages[I].name, ret,
            ret == 0x14 ? " (block missing)" : ret == 1 ? " (version not listed)" : "");
        return ret;
    }

    void hookGvmStages(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        static const mach_vm_address_t wrappers[] = {reinterpret_cast<mach_vm_address_t>(wrapGvmStage<0>),
            reinterpret_cast<mach_vm_address_t>(wrapGvmStage<1>), reinterpret_cast<mach_vm_address_t>(wrapGvmStage<2>),
            reinterpret_cast<mach_vm_address_t>(wrapGvmStage<3>)};
        static_assert(arrsize(wrappers) == arrsize(gvmStages));
        for (size_t i = 0; i < arrsize(gvmStages); i++) {
            auto&      stage = gvmStages[i];
            const auto at    = findUnique(stage.pattern, nullptr, stage.length, slide, size);
            if (at == 0) {
                BCLOG("BC250HWL", "GVM stage %s not found; not logged", stage.name);
                continue;
            }
            KernelPatcher::RouteRequest request {nullptr, wrappers[i], stage.org};
            request.from = at;
            if (!patcher.routeMultiple(id, &request, 1)) {
                BCLOG("BC250HWL", "GVM stage %s failed to route", stage.name);
                patcher.clearError();
            }
        }
    }

    // PSP's sw_init (psp.c 0xc353e98) classifies MP0's version (0xc353fec, into its context at +0x7D94) and fails on
    // an unknown one; 11.0.x accepts revisions 0, 2-5, 7 and 9-13, not the BC-250's 8. Linux drives PSP 11.0.8 with
    // its own ring-only code, whose ring setup uses the same C2PMSG registers as 11.0.0, and 11.0.0's bootloader steps
    // are skipped once the SOS is running (as it is on the BC-250 at boot). PSP's sw_init itself is software only (it
    // installs the PSP 11 function table); the PSP is first driven at hw_init. From level 10 the classifier alone is
    // given MP0 as 11.0.0 (Navi 10), on a copy of the discovery entry, so bgm_create's MP0 stage is unchanged.
    const UInt8 kPspClassifyPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xF6, 0x48, 0x89, 0xFB,
        0x48, 0x83, 0xC7, 0x30, 0xBA, 0x60, 0x02, 0x00, 0x00};
    constexpr size_t kPspType = 0x7D94;

    mach_vm_address_t orgPspClassify = 0;

    UInt32 wrapPspClassify(UInt8* psp, const UInt8* entry)
    {
        UInt8      copy[kTopologyEntrySize];
        const auto major = getMember<UInt32>(const_cast<UInt8*>(entry), 0x8);
        const auto minor = getMember<UInt32>(const_cast<UInt8*>(entry), 0xC);
        const auto rev   = getMember<UInt32>(const_cast<UInt8*>(entry), 0x10);
        const UInt8* use = entry;
        if (major == 11 && minor == 0 && rev == 8) {
            memcpy(copy, entry, sizeof(copy));
            getMember<UInt32>(copy, 0x10) = 0;
            use = copy;
        }
        const auto ret = FunctionCast(wrapPspClassify, orgPspClassify)(psp, use);
        BCLOG("BC250HWL", "PSP classify: MP0 %u.%u.%u%s -> type 0x%X, ret 0x%X", major, minor, rev,
            use == copy ? " (given as 11.0.0)" : "", getMember<UInt32>(psp, kPspType), ret);
        return ret;
    }

    void hookPspClassify(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        const auto at = findUnique(kPspClassifyPattern, nullptr, sizeof(kPspClassifyPattern), slide, size);
        if (at == 0) {
            BCLOG("BC250HWL", "PSP classifier not found; MP0 not remapped");
            return;
        }
        KernelPatcher::RouteRequest request {nullptr, wrapPspClassify, orgPspClassify};
        request.from = at;
        if (!patcher.routeMultiple(id, &request, 1)) {
            BCLOG("BC250HWL", "PSP classifier failed to route");
            patcher.clearError();
        }
    }

    void** ripTarget(mach_vm_address_t insn)
    {
        const auto disp = getMember<SInt32>(reinterpret_cast<void*>(insn), kRipLoadDisp);
        return reinterpret_cast<void**>(insn + kRipLoadLength + disp);
    }

    UInt64 wrapSwipProviderLookup(UInt32 swip, void** out)
    {
        const auto ret = FunctionCast(wrapSwipProviderLookup, orgSwipProviderLookup)(swip, out);
        const auto fn  = reinterpret_cast<mach_vm_address_t>(swipProviderFn ? *swipProviderFn : nullptr);
        if (fn == 0) {
            BCLOG("BC250HWL", "SWIP %u (%s) provider lookup: no provider registered -> default table", swip,
                swipName(swip));
            return ret;
        }
        mach_vm_address_t offset = 0;
        const char*       where  = kextOf(fn, &offset);
        if ((ret & 0xFF) == 0) {
            BCLOG("BC250HWL", "SWIP %u (%s) provider lookup: provider %s+0x%llX (ctx %p) declined -> default table",
                swip, swipName(swip), where, offset, swipProviderCtx ? *swipProviderCtx : nullptr);
            return ret;
        }
        BCLOG("BC250HWL", "SWIP %u (%s) provider lookup: provider %s+0x%llX answered: sw_init %p, %p, %p, %p", swip,
            swipName(swip), where, offset, out ? out[0] : nullptr, out ? out[1] : nullptr, out ? out[2] : nullptr,
            out ? out[3] : nullptr);
        return ret;
    }

    bool swipAllowedToInit(UInt32 swip)
    {
        for (UInt32 i = 0; i < arrsize(kSwipBootOrder) && i < swipAllowed; i++) {
            if (kSwipBootOrder[i] == swip) { return true; }
        }
        return false;
    }

    // Level 11: SWIPs answered without running their code.
    //  - SMU: Apple's SMU code has no backend for 11.0.8, and the Navi 10 one speaks a message set the BC-250's SMU
    //    interprets differently. Linux's Cyan Skillfish SMU driver sends nothing at init beyond version queries and
    //    the driver table address; it runs with cg/pg flags 0, no GFXOFF, no power-up messages, and the SMU firmware
    //    is already running at boot. Other SWIPs reach the SMU only through these events and
    //    ipiSmuQueryInstanceInfo, which reads the IPI context (created regardless).
    //  - VCN, JPEG: not usable on the BC-250 (Linux's SMU driver has no VCN/JPEG power-up, so VCN stays gated);
    //    running their hw_init would touch a powered-off block.
    // Every event to a stubbed SWIP returns success without running any code; its position in the boot order still
    // counts for bc250swip.
    constexpr UInt32 kStubbedSwips[] = {8, 9, 15};

    // Level 22: MES stubbed as well. Apple's MES hw_init (mes_set_resources_10_1, 0xc34eb19, through ipi_mes +0x18)
    // is no hardware setup: it sends SET_RESOURCES to MES firmware it expects running and polls the fence with
    // "while (wait()) ;" (0xc35243a gives up after 2000 polls, the caller retries forever), so without MES firmware it
    // never returns. The BC-250 has none (no cyan_skillfish2 MES in linux-firmware; Linux runs GFX10 without MES), no
    // code of GC's starts the MES pipe (no CP_MES_* write in level 19), and Apple's accelerator schedules through its
    // own AMDSWScheduler and maps queues through the KIQ (AMDGFX10KIQHWChannel::submitSetResourcesPacket), as Linux.
    constexpr UInt32 kSwipMes = 14;

    // Level 12: hw_init (event 1) runs one SWIP at a time: only for the first bc250hw=K (default 1) SWIPs of the boot
    // order, and hw_fini (event 3) only for those. From this level every sw_init runs by default (bc250swip=10).
    // DMCU is stubbed as well: Linux runs DCN 2.0.1 without a DMCU (dcn201_resource.c: disable_dmcu, no DMCU/ABM
    // created, no DMCU register touched), and Apple's DMCU hw_init (0xc30b0e2) reads and writes DMCU registers
    // unconditionally. Its sw_init skipped them for DMU 2.0.3 and is covered by levels 10-11.
    constexpr UInt32 kSwipDmcu = 10, kSwipEventHwInit = 1, kSwipEventHwFini = 3;

    UInt32 hwAllowed       = 1;
    constexpr UInt32 kGvmHwPosition = 2;    // GVM is second in the boot order.
    constexpr UInt32 kPspHwPosition = 3;    // PSP is third.
    constexpr UInt32 kGcHwPosition  = 5;    // GC is fifth (after the stubbed SMU).
    constexpr UInt32 kSdmaHwPosition = 6;   // SDMA is sixth.
    constexpr UInt32 kAllHwPosition  = 10;  // MES and the rest are stubs from level 22.

    // The furthest SWIP of the boot order a level lets run hw_init: GVM from level 13, PSP from 15, GC from 18, SDMA
    // from 20.
    UInt32 hwLimit() { return kAllHwPosition; }
    UInt32 swipHwInitialised = 0;    // Bit per SWIP id whose hw_init ran.

    bool swipStubbed(UInt32 swip)
    {
        if (swip == kSwipDmcu || swip == kSwipMes) { return true; }
        for (const auto s : kStubbedSwips) {
            if (s == swip) { return true; }
        }
        return false;
    }

    bool swipAllowedToHwInit(UInt32 swip)
    {
        for (UInt32 i = 0; i < arrsize(kSwipBootOrder) && i < hwAllowed; i++) {
            if (kSwipBootOrder[i] == swip) { return true; }
        }
        return false;
    }

    bool swipBit(UInt32 mask, UInt32 swip) { return swip < 32 && (mask & (1U << swip)) != 0; }

    UInt64 wrapIpiSwipEvent(void* ipi, UInt32 swip, UInt32 event, void* context)
    {
        if (swipStubbed(swip) && (event != kSwipEventSwInit || swipAllowedToInit(swip)) &&
            (event != kSwipEventHwInit || swipAllowedToHwInit(swip)))
        {
            BCLOG("BC250HWL", "SWIP %u (%s) event %u: stubbed, returning success", swip, swipName(swip), event);
            if (event == kSwipEventSwInit && swip < 32) { swipInitialised |= 1U << swip; }
            if (event == kSwipEventHwInit && swip < 32) { swipHwInitialised |= 1U << swip; }
            return 1;
        }
        bool run = false;
        if (event == kSwipEventSwInit) {
            run = swipAllowedToInit(swip);
        }
        else if (event == kSwipEventSwFini) {
            run = swipBit(swipInitialised, swip);
        }
        else if (event == kSwipEventHwInit) {
            run = swipAllowedToHwInit(swip) && swipBit(swipInitialised, swip);
        }
        else if (event == kSwipEventHwFini) {
            run = swipBit(swipHwInitialised, swip);
        }
        if (!run) {
            BCLOG("BC250HWL", "SWIP %u (%s) event %u: refused (bc250swip=%u, bc250hw=%u)", swip, swipName(swip), event,
                swipAllowed, hwAllowed);
            return 0;
        }
        BCLOG("BC250HWL", "SWIP %u (%s) event %u >>>", swip, swipName(swip), event);
        const auto ret = FunctionCast(wrapIpiSwipEvent, orgIpiSwipEvent)(ipi, swip, event, context);
        BCLOG("BC250HWL", "SWIP %u (%s) event %u <<< 0x%llX", swip, swipName(swip), event, ret);
        if ((ret & 0xFF) != 0 && swip < 32) {
            if (event == kSwipEventSwInit) { swipInitialised |= 1U << swip; }
            if (event == kSwipEventHwInit) { swipHwInitialised |= 1U << swip; }
        }
        return ret;
    }

    // Level 12: CAIL reads its settings by name from the GPU's IORegistry properties (as PP_Log* are set in
    // DebugEnabler); the names come from the table at 0xc858c20. BGM's hw_init has three steps Linux does not take
    // on the BC-250, each gated by one:
    //  - SetExtendedTagEn (0x0E, default on): PCIE hw_init sets bit 8 at a hard-coded PCI config offset 0x60 of
    //    device records 7 and 8 (Device Control of a Navi 10 card's internal switch ports). The BC-250 has no such
    //    switch and its GPU's PCIe capability is at 0x64, so the write would land elsewhere: off.
    //  - DisableNbioMediumGrainClockGating (0x12), DisableNbioMediumGrainLightSleep (0x13): Linux runs with
    //    cg_flags = 0: on.
    //  - DisableRomMediumGrainClockGating (0x14): Linux skips ROM clock gating on APUs: on.
    struct CailSetting
    {
        const char* name;
        UInt32      value;
    };
    constexpr CailSetting kCailSettings[] = {
        {"SetExtendedTagEn", 0},
        {"DisableNbioMediumGrainClockGating", 1},
        {"DisableNbioMediumGrainLightSleep", 1},
        {"DisableRomMediumGrainClockGating", 1},
    };

    // Level 13: GVM's HDP hw_init enables HDP memory light sleep/deep sleep/shutdown unless the flags from
    // DisableHdpClockPowerGating (bits 1-3) say otherwise; with all set it only toggles the clock overrides around a
    // disable, as Linux leaves HDP (cg_flags = 0).
    constexpr CailSetting kCailSettingsGvm[] = {
        {"DisableHdpClockPowerGating", 0xF},
    };

    void setCailSettings()
    {
        auto* gpu = NRed::singleton().getIGPU();
        if (gpu == nullptr) {
            BCLOG("BC250HWL", "CAIL settings: no GPU device; not set");
            return;
        }
        for (const auto& s : kCailSettings) {
            NRed::singleton().setProp32(s.name, s.value);
            BCLOG("BC250HWL", "CAIL setting %s = %u", s.name, s.value);
        }
        for (const auto& s : kCailSettingsGvm) {
            NRed::singleton().setProp32(s.name, s.value);
            BCLOG("BC250HWL", "CAIL setting %s = 0x%X", s.name, s.value);
        }
    }

    // The extended-tag write itself (0xc54d988: pcie, device record): read 2 bytes at PCI config 0x60, set bit 8,
    // write back. From level 12 it is also replaced by a logged no-op, so the write cannot happen even if CAIL does
    // not pick up SetExtendedTagEn.
    const UInt8 kPcieSetExtTagPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x10, 0x89, 0xF3,
        0x49, 0x89, 0xFE, 0x4C, 0x8D, 0x45, 0xE8, 0x41, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x3F, 0xBA,
        0x60, 0x00, 0x00, 0x00};

    mach_vm_address_t orgPcieSetExtTag = 0;

    void wrapPcieSetExtTag(void* pcie, UInt32 record)
    {
        BCLOG("BC250HWL", "PCIE: extended-tag write to PCI config 0x60 of device record %u skipped (%p)", record, pcie);
    }

    // CAIL's setting reader (0xc53ac97: ctx, id, default, out), logged from level 12 so the log shows what CAIL
    // actually read.
    const UInt8 kCailReadSettingPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
        0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB, 0x41, 0x89, 0xD6, 0x49, 0x89, 0xFF, 0x4C, 0x8D, 0x65, 0xDC, 0x41, 0xC7,
        0x04, 0x24, 0x00, 0x00, 0x00, 0x00, 0x89, 0xF7};
    constexpr UInt32 kCailSettingLogMax = 80;

    mach_vm_address_t orgCailReadSetting = 0;
    UInt32            cailReads          = 0;

    void wrapCailReadSetting(void* ctx, UInt32 id, UInt32 defaultValue, UInt32* out)
    {
        FunctionCast(wrapCailReadSetting, orgCailReadSetting)(ctx, id, defaultValue, out);
        if (++cailReads <= kCailSettingLogMax) {
            BCLOG("BC250HWL", "CAIL setting 0x%02X read: %u (default %u)", id, out ? *out : 0, defaultValue);
        }
    }

    // Level 13: GVM's hw_init as a dry run. gvm.c's hw_init (0xc31f3e9: gvm, input, output) runs golden settings (per
    // UMC/MMHUB/HDP/ATHUB, from a CGS query that returns none on Navi 10's path), then the hw_init of the UMC (8.0.0:
    // software only), HDP (5.0.0: HDP_NONSURFACE_INFO, clock/memory power gating, MISC_CNTL), GC hub (10.1.10: VM
    // apertures, GART, L2, contexts) and ATHUB (2.0.0) tables. GVM reaches the hardware only through its CGS wrappers
    // (gvm_cgs.c): register write/write64/indirect write/indirect write64, reads, GPU memory alloc and copy. At this
    // level every write and memory copy is logged and not made (reported as done), reads and allocations run and are
    // logged, and so are GVM's setting reads by name and the golden-settings query. The result is the exact list of
    // what GVM would program on the BC-250, to be checked against Linux's gmc_v10_0/gfxhub_v2_0/mmhub_v2_0/hdp_v5_0
    // before any of it is let through. HDP memory power gating is turned off by setting (Linux: cg_flags = 0).
    const UInt8 kGvmWritePattern[]      = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x87, 0x48, 0x0E, 0x00, 0x00, 0x49, 0x8B,
             0x40};
    const UInt8 kGvmWrite64Pattern[]    = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x89, 0xCB,
           0x49, 0x89, 0xFC, 0x48, 0x8B, 0x8F, 0x48, 0x0E, 0x00, 0x00, 0x48, 0x8B, 0x41};
    const UInt8 kGvmWriteIndPattern[]   = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x87, 0x48, 0x0E, 0x00, 0x00, 0x49, 0x8B,
          0x80};
    const UInt8 kGvmWriteInd64Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x89,
        0xCB, 0x49, 0x89, 0xFC, 0x48, 0x8B, 0x8F, 0x48, 0x0E, 0x00, 0x00, 0x48, 0x8B, 0x81};
    const UInt8 kGvmMemCopyPattern[]    = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
           0x48, 0x83, 0xEC, 0x18, 0x48, 0x89, 0xCB, 0x49};
    const UInt8 kGvmReadPattern[]       = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x8F, 0x48, 0x0E, 0x00, 0x00, 0x48, 0x8B,
              0x41};
    const UInt8 kGvmRead64Pattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49, 0x89,
            0xFE, 0x48, 0x8B, 0x8F, 0x48, 0x0E, 0x00, 0x00, 0x48, 0x8B, 0x41};
    const UInt8 kGvmAllocPattern[]      = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
             0x48, 0x83, 0xEC, 0x48, 0x4C};
    const UInt8 kGvmKeyReadPattern[]    = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x28,
           0xC7, 0x45, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x41};
    const UInt8 kGvmGoldenPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x48, 0xC7, 0x45, 0xE0,
            0x00, 0x00, 0x00, 0x00, 0x89, 0x75, 0xE8, 0x89, 0x55, 0xEC, 0x89, 0x4D, 0xF0, 0x48, 0x8B, 0x8F};
    const UInt8 kGvmHwInitPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0xBC,
            0x02, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF, 0x0F};
    constexpr UInt32 kGvmWriteLogMax = 400, kGvmReadLogMax = 200, kGvmOtherLogMax = 60, kGvmInputDwords = 24;

    mach_vm_address_t orgGvmWrite = 0, orgGvmWrite64 = 0, orgGvmWriteInd = 0, orgGvmWriteInd64 = 0,
                      orgGvmMemCopy = 0, orgGvmRead = 0, orgGvmRead64 = 0, orgGvmAlloc = 0, orgGvmKeyRead = 0,
                      orgGvmGolden = 0, orgGvmHwInit = 0;
    UInt32 gvmWrites = 0, gvmReads = 0, gvmOthers = 0;

    bool gvmLogWrite() { return ++gvmWrites <= kGvmWriteLogMax; }
    bool gvmLogRead() { return ++gvmReads <= kGvmReadLogMax; }
    bool gvmLogOther() { return ++gvmOthers <= kGvmOtherLogMax; }

    // Level 14: GVM's register writes and GPU memory copies are made. The level-13 dry run on the board showed they
    // are Linux's gfxhub/mmhub_v2_0 GART enable (AGP off, system aperture = the FB, L2/L1 TLB, context 0 = GART),
    // except that Apple's code, written for cards whose FB starts at physical 0, gives the hubs VRAM offsets where
    // physical addresses are needed. The BC-250's FB is a carve-out at GCMC_VM_FB_OFFSET (0x450000000); Linux adds it
    // (vram_base_offset) to the page-table base and the default/dummy page addresses. Level 14 does the same for
    // every such register pair of both hubs, when the value is inside the FB: the write to the low half is adjusted
    // and the carry goes to the later write to the same pair's high half. Write64 and indirect writes, not used by
    // GVM's hw_init on the board, stay logged and not made. On the board the GART table's fill (GPU memory copies
    // within it) falls back to GVM's own copy through the CP's register-driven DMA (GRBM_GFX_CNTL, uconfig
    // 0x2063-0x2067/0x20E8, CP_STAT polled), with FB MC addresses in the system aperture.
    struct GvmAddrPair
    {
        UInt32 lo, hi;
        UInt32 shift;    // 0: byte address (flags in bits 0-11), 12: page number.
    };
    constexpr UInt32 kGcBase = 0x1260, kMmhubBase = 0x1A000, kGvmContexts = 16;
    constexpr UInt32 kGcContext0Base = kGcBase + 0x168B, kMmContext0Base = kMmhubBase + 0x72B;
    constexpr GvmAddrPair kGvmAddrPairs[] = {
        {kGcBase + 0x170C, kGcBase + 0x170D, 12},        // GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB/MSB
        {kMmhubBase + 0x858, kMmhubBase + 0x859, 12},    // MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB/MSB
        {kGcBase + 0x15EF, kGcBase + 0x15F0, 12},        // GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32/HI32
        {kMmhubBase + 0x68F, kMmhubBase + 0x690, 12},    // MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32/HI32
    };

    // GCVM/MMVM_CONTEXT0_CNTL and their RETRY_PERMISSION_OR_INVALID_PAGE_FAULT bit.
    constexpr UInt32 kGcContext0Cntl = kGcBase + 0x1620, kMmContext0Cntl = kMmhubBase + 0x6C0, kContextRetry = 1U << 7;

    bool   gvmLive      = false;
    bool   gvmNoRetry   = false;    // Level 27: context 0 without fault retry, as Linux.
    UInt64 gvmFbOffset  = 0, gvmFbSize = 0, gvmFbBase = 0;    // gvmFbBase: the FB's MC address.
    UInt32 gvmAdjusted  = 0;

    // High halves awaiting their carry, one per pair: Apple interleaves the hubs (GC low, MMHUB low, GC high, MMHUB
    // high), so a single pending entry lost the GC one on the board.
    struct GvmPendingHi
    {
        UInt32 hi, add;    // hi 0: free.
    };
    GvmPendingHi gvmPending[8] {};

    GvmPendingHi* gvmPendingFor(UInt32 hi, bool create)
    {
        GvmPendingHi* free = nullptr;
        for (auto& p : gvmPending) {
            if (p.hi == hi) { return &p; }
            if (p.hi == 0 && free == nullptr) { free = &p; }
        }
        return create ? free : nullptr;
    }

    bool gvmAddrPair(UInt32 reg, GvmAddrPair* pair)
    {
        for (const auto& p : kGvmAddrPairs) {
            if (reg == p.lo || reg == p.hi) {
                *pair = p;
                return true;
            }
        }
        constexpr UInt32 kContext0Bases[] = {kGcContext0Base, kMmContext0Base};
        for (const auto base : kContext0Bases) {
            if (reg >= base && reg < base + 2 * kGvmContexts) {
                const auto lo = base + ((reg - base) & ~1U);
                *pair         = {lo, lo + 1, 0};
                return true;
            }
        }
        return false;
    }

    UInt32 gvmAdjustAddress(UInt32 reg, UInt32 value)
    {
        GvmAddrPair pair;
        if (!gvmAddrPair(reg, &pair)) { return value; }
        if (reg == pair.lo) {
            if (auto* stale = gvmPendingFor(pair.hi, false)) { stale->hi = 0; }
            const UInt64 address =
                pair.shift ? static_cast<UInt64>(value) << pair.shift : static_cast<UInt64>(value) & ~0xFFFULL;
            if (address >= gvmFbSize) { return value; }
            auto* pending = gvmPendingFor(pair.hi, true);
            if (pending == nullptr) {
                // Cannot carry into the high half: leave the pair as Apple wrote it rather than half-adjusted.
                BCLOG("BC250HWL", "GVM: no slot for the high half of 0x%05X; not adjusted", reg);
                return value;
            }
            const UInt64 add = gvmFbOffset >> pair.shift;
            const UInt64 sum = static_cast<UInt64>(value) + (add & 0xFFFFFFFF);
            pending->hi      = pair.hi;
            pending->add     = static_cast<UInt32>((add >> 32) + (sum >> 32));
            gvmAdjusted++;
            return static_cast<UInt32>(sum);
        }
        auto* pending = gvmPendingFor(reg, false);
        if (pending == nullptr) { return value; }
        pending->hi = 0;
        return value + pending->add;
    }

    // Registers are dword offsets with the block's base already added; ip is the block (0x0B GC, 0x1B MMHUB, 0x22
    // HDP, ...).
    UInt64 wrapGvmWrite(void* gvm, UInt32 reg, UInt32 value, UInt32 ip)
    {
        if (!gvmLive) {
            if (gvmLogWrite()) { BCLOG("BC250HWL", "GVM dry: W 0x%05X = 0x%08X (ip 0x%02X)", reg, value, ip); }
            return 0;
        }
        // Apple's GVM enables retry on context 0 (0x01555481); Linux's gfxhub/mmhub_v2_0_enable_system_domain clears
        // it, so a bad VMID0 translation faults (status and address logged) instead of retrying forever in the UTCL2.
        if (gvmNoRetry && (reg == kGcContext0Cntl || reg == kMmContext0Cntl) && (value & kContextRetry) != 0) {
            BCLOG("BC250HWL", "GVM: W 0x%05X = 0x%08X (ip 0x%02X), Apple's 0x%08X without fault retry", reg,
                value & ~kContextRetry, ip, value);
            return FunctionCast(wrapGvmWrite, orgGvmWrite)(gvm, reg, value & ~kContextRetry, ip);
        }
        const auto written = gvmAdjustAddress(reg, value);
        if (gvmLogWrite()) {
            if (written != value) {
                BCLOG("BC250HWL", "GVM: W 0x%05X = 0x%08X (ip 0x%02X), Apple's 0x%08X + FB offset", reg, written, ip,
                    value);
            }
            else {
                BCLOG("BC250HWL", "GVM: W 0x%05X = 0x%08X (ip 0x%02X)", reg, written, ip);
            }
        }
        return FunctionCast(wrapGvmWrite, orgGvmWrite)(gvm, reg, written, ip);
    }

    UInt64 wrapGvmWrite64(void* gvm, UInt32 reg, UInt64 value, UInt32 ip)
    {
        (void)gvm;
        if (gvmLogWrite()) { BCLOG("BC250HWL", "GVM dry: W64 0x%05X = 0x%016llX (ip 0x%02X)", reg, value, ip); }
        return 0;
    }

    UInt64 wrapGvmWriteInd(void* gvm, UInt64 a, UInt64 b, UInt64 c, UInt64 d)
    {
        (void)gvm;
        if (gvmLogWrite()) { BCLOG("BC250HWL", "GVM dry: indirect W (0x%llX, 0x%llX, 0x%llX, 0x%llX)", a, b, c, d); }
        return 0;
    }

    UInt64 wrapGvmWriteInd64(void* gvm, UInt64 a, UInt64 b, UInt64 c, UInt64 d)
    {
        (void)gvm;
        if (gvmLogWrite()) { BCLOG("BC250HWL", "GVM dry: indirect W64 (0x%llX, 0x%llX, 0x%llX, 0x%llX)", a, b, c, d); }
        return 0;
    }

    // gvm_cgs_gpu_memory_copy(gvm, a, b, size): success is 1.
    UInt8 wrapGvmMemCopy(void* gvm, UInt64 a, UInt64 b, UInt64 size)
    {
        if (!gvmLive) {
            if (gvmLogWrite()) {
                BCLOG("BC250HWL", "GVM dry: GPU memory copy 0x%llX <- 0x%llX, 0x%llX bytes", b, a, size);
            }
            return 1;
        }
        const auto ret = FunctionCast(wrapGvmMemCopy, orgGvmMemCopy)(gvm, a, b, size);
        if (gvmLogWrite()) {
            BCLOG("BC250HWL", "GVM: GPU memory copy 0x%llX <- 0x%llX, 0x%llX bytes -> %u", b, a, size, ret);
        }
        return ret;
    }

    UInt32 wrapGvmRead(void* gvm, UInt32 reg, UInt32 ip)
    {
        const auto value = FunctionCast(wrapGvmRead, orgGvmRead)(gvm, reg, ip);
        if (gvmLogRead()) { BCLOG("BC250HWL", "GVM dry: R 0x%05X -> 0x%08X (ip 0x%02X)", reg, value, ip); }
        return value;
    }

    UInt64 wrapGvmRead64(void* gvm, UInt32 reg, UInt32 ip)
    {
        const auto value = FunctionCast(wrapGvmRead64, orgGvmRead64)(gvm, reg, ip);
        if (gvmLogRead()) { BCLOG("BC250HWL", "GVM dry: R64 0x%05X -> 0x%016llX (ip 0x%02X)", reg, value, ip); }
        return value;
    }

    // gvm_cgs_alloc_memory(gvm, size, alignment, type, out, out, out on the stack): returns the handle.
    UInt64 wrapGvmAlloc(void* gvm, UInt64 size, UInt32 alignment, UInt32 type, UInt64* out1, UInt64* out2,
        UInt64* out3)
    {
        const auto handle = FunctionCast(wrapGvmAlloc, orgGvmAlloc)(gvm, size, alignment, type, out1, out2, out3);
        if (gvmLogOther()) {
            BCLOG("BC250HWL", "GVM dry: alloc 0x%llX bytes (align 0x%X, type %u) -> handle 0x%llX, 0x%llX, 0x%llX, 0x%llX",
                size, alignment, type, handle, out1 ? *out1 : 0, out2 ? *out2 : 0, out3 ? *out3 : 0);
        }
        return handle;
    }

    // Setting read by name (gvm, name, default, out, count): true if read.
    UInt8 wrapGvmKeyRead(void* gvm, const char* name, UInt32 defaultValue, UInt32* out, UInt32 count)
    {
        const auto ret = FunctionCast(wrapGvmKeyRead, orgGvmKeyRead)(gvm, name, defaultValue, out, count);
        if (gvmLogOther()) {
            BCLOG("BC250HWL", "GVM setting %s read: %u (default %u, ret %u)", name ? name : "(null)", out ? *out : 0,
                defaultValue, ret);
        }
        return ret;
    }

    void* wrapGvmGolden(void* gvm, UInt32 type, UInt32 major, UInt32 minor)
    {
        const auto table = FunctionCast(wrapGvmGolden, orgGvmGolden)(gvm, type, major, minor);
        if (gvmLogOther()) {
            BCLOG("BC250HWL", "GVM golden settings for 0x%02X %u.%u: %p", type, major, minor, table);
        }
        return table;
    }

    UInt32 wrapGvmHwInit(void* gvm, const UInt32* input, void* output)
    {
        if (input != nullptr) {
            for (UInt32 i = 0; i < kGvmInputDwords; i += 4) {
                BCLOG("BC250HWL", "GVM hw_init input +0x%02X: %08X %08X %08X %08X", i * 4, input[i], input[i + 1],
                    input[i + 2], input[i + 3]);
            }
        }
        const auto ret = FunctionCast(wrapGvmHwInit, orgGvmHwInit)(gvm, input, output);
        BCLOG("BC250HWL", "GVM hw_init (%s) <<< 0x%X after %u write(s) (%u address(es) given the FB offset), %u read(s)",
            gvmLive ? "live" : "dry run", ret, gvmWrites, gvmAdjusted, gvmReads);
        return ret;
    }

    // Level 14: GVM's writes are made once the FB's system address and size are known (FB location in 16 MB units,
    // GCMC_VM_FB_LOCATION_BASE/TOP); otherwise the dry run stays.
    void enableGvmLive()
    {
        constexpr UInt32 kFbLocationBase = kGcBase + 0x1720, kFbLocationTop = kGcBase + 0x1721;
        auto&            nred = NRed::singleton();
        const auto       base = nred.readReg32(kFbLocationBase) & 0xFFFFFF;
        const auto       top  = nred.readReg32(kFbLocationTop) & 0xFFFFFF;
        gvmFbOffset           = nred.getFbOffset();
        gvmFbSize             = top >= base ? static_cast<UInt64>(top - base + 1) << 24 : 0;
        gvmFbBase             = static_cast<UInt64>(base) << 24;
        if (gvmFbOffset == 0 || (gvmFbOffset & 0xFFFFF) != 0 || gvmFbSize == 0 || gvmFbSize > (16ULL << 30)) {
            BCLOG("BC250HWL", "level 14: FB offset 0x%llX, size 0x%llX not usable; GVM stays a dry run", gvmFbOffset,
                gvmFbSize);
            return;
        }
        gvmLive = true;
        BCLOG("BC250HWL", "level 14: GVM writes are made; FB offset 0x%llX added to its addresses below 0x%llX",
            gvmFbOffset, gvmFbSize);
        UInt32 retry = 0;
        PE_parse_boot_argn("bc250retry", &retry, sizeof(retry));
        gvmNoRetry = retry == 0;
        BCLOG("BC250HWL", "level 27: VM context 0 fault retry %s", gvmNoRetry ? "off (as Linux)" : "on (Apple's)");
    }

    // Returns false if a write path could not be hooked; GVM's hw_init must then not run.
    bool hookGvmDryRun(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        struct Hook
        {
            const char*        name;
            const UInt8*       pattern;
            size_t             length;
            mach_vm_address_t  wrapper;
            mach_vm_address_t& org;
            bool               guard;
        };
        Hook hooks[] = {
            {"write", kGvmWritePattern, sizeof(kGvmWritePattern), reinterpret_cast<mach_vm_address_t>(wrapGvmWrite),
                orgGvmWrite, true},
            {"write64", kGvmWrite64Pattern, sizeof(kGvmWrite64Pattern),
                reinterpret_cast<mach_vm_address_t>(wrapGvmWrite64), orgGvmWrite64, true},
            {"indirect write", kGvmWriteIndPattern, sizeof(kGvmWriteIndPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGvmWriteInd), orgGvmWriteInd, true},
            {"indirect write64", kGvmWriteInd64Pattern, sizeof(kGvmWriteInd64Pattern),
                reinterpret_cast<mach_vm_address_t>(wrapGvmWriteInd64), orgGvmWriteInd64, true},
            {"memory copy", kGvmMemCopyPattern, sizeof(kGvmMemCopyPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGvmMemCopy), orgGvmMemCopy, true},
            {"read", kGvmReadPattern, sizeof(kGvmReadPattern), reinterpret_cast<mach_vm_address_t>(wrapGvmRead),
                orgGvmRead, false},
            {"read64", kGvmRead64Pattern, sizeof(kGvmRead64Pattern), reinterpret_cast<mach_vm_address_t>(wrapGvmRead64),
                orgGvmRead64, false},
            {"alloc", kGvmAllocPattern, sizeof(kGvmAllocPattern), reinterpret_cast<mach_vm_address_t>(wrapGvmAlloc),
                orgGvmAlloc, false},
            {"setting read", kGvmKeyReadPattern, sizeof(kGvmKeyReadPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGvmKeyRead), orgGvmKeyRead, false},
            {"golden query", kGvmGoldenPattern, sizeof(kGvmGoldenPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGvmGolden), orgGvmGolden, false},
            {"hw_init", kGvmHwInitPattern, sizeof(kGvmHwInitPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGvmHwInit),
                orgGvmHwInit, false},
        };
        // Find every write path first: route none of them unless all are there.
        mach_vm_address_t at[arrsize(hooks)] {};
        for (size_t i = 0; i < arrsize(hooks); i++) {
            at[i] = findUnique(hooks[i].pattern, nullptr, hooks[i].length, slide, size);
            if (at[i] == 0) {
                BCLOG("BC250HWL", "level 13: GVM %s not found", hooks[i].name);
                if (hooks[i].guard) { return false; }
            }
        }
        for (size_t i = 0; i < arrsize(hooks); i++) {
            if (at[i] == 0) { continue; }
            KernelPatcher::RouteRequest request {nullptr, hooks[i].wrapper, hooks[i].org};
            request.from = at[i];
            if (!patcher.routeMultiple(id, &request, 1)) {
                BCLOG("BC250HWL", "level 13: GVM %s failed to route", hooks[i].name);
                patcher.clearError();
                if (hooks[i].guard) { return false; }
            }
        }
        return true;
    }

    // Level 15: PSP's hw_init as a dry run. psp.c's hw_init (0xc3544ba: psp, input, output) allocates its buffers,
    // then psp_hardware_initialization (0xc354953): bootloader ECC mode (only for input+0x18 modes 1-3), key DB,
    // sysdrv and SOS loads (each skipped while C2PMSG_81 is non-zero, i.e. the SOS runs, as on the BC-250 at boot),
    // KM ring create (psp_ring_create_11_0 0xc361e0f: wait C2PMSG_64 bit 31, ring address/size to C2PMSG_69-71,
    // ring type to C2PMSG_64, wait for the response; Linux's psp_v11_0 bare-metal path), then TMR init/load, ASD,
    // DTM, RAS/WSPE/HDCP/security caps/AUC/FP/XGMI resumes and ring interrupts, each a GFX command through
    // psp_cmd_km_submit (0xc3582da: psp, command, response, handle; NootRX's firmware hook point). The BC-250's
    // SOS has no ASD or TAs (Linux loads none on PSP 11.0.8), and the firmware images PSP would load are Navi 10's.
    // At this level every PSP register write (0xc357651: psp, reg, base index, value, ip) is logged and not made,
    // reads (0xc35762a) run and are logged, the ring create is logged and reported done, and every command is logged
    // (id and first dwords) and reported done without being submitted. The result is the list of commands PSP would
    // send, to be matched against Linux's PSP 11.0.8 sequence before any reaches the SOS.
    const UInt8 kPspWritePattern[]      = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x4F, 0x08, 0x49, 0x8B, 0x41, 0x38, 0x48,
             0x85, 0xC0, 0x74, 0x12};
    const UInt8 kPspReadPattern[]       = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x47, 0x08, 0x49, 0x8B, 0x40, 0x40, 0x48,
              0x85, 0xC0, 0x74, 0x0F};
    const UInt8 kPspRingCreatePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
        0x48, 0x83, 0xEC, 0x28, 0x41, 0x89, 0xF7, 0x48};
    const UInt8 kPspCmdSubmitPattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
         0x48, 0x83, 0xEC, 0x18, 0x49, 0x89, 0xCD};
    const UInt8 kPspHwInitPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
            0x50, 0xC7, 0x45, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x41};
    const UInt8 kPspHwFiniPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x10, 0xC7, 0x45,
            0xEC, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF, 0x0F};
    constexpr UInt32 kPspWriteLogMax = 200, kPspReadLogMax = 150, kPspCmdLogMax = 40;
    constexpr UInt32 kPspInputDwords = 16;

    mach_vm_address_t orgPspWrite = 0, orgPspRead = 0, orgPspRingCreate = 0, orgPspCmdSubmit = 0, orgPspHwInit = 0,
                      orgPspHwFini = 0;
    UInt32 pspWrites = 0, pspReads = 0, pspCmds = 0;

    // Level 16: PSP's ring and TMR are made, with Linux's PSP 11.0.8 sequence as the reference (the dry run showed
    // the differences):
    //  - no TOC: the BC-250 has none (Linux: no TOC, TMR PSP_TMR_SIZE = 4 MB). hw_init's input (+0x20 size, +0x28
    //    image) is passed on a copy without it, so Apple's TMR init keeps its 4 MB default and sends no LOAD_TOC;
    //  - commands: SETUP_TMR (5) and DESTROY_TMR (7) are submitted, the TMR only if it lies inside the FB and clear
    //    of its first 64 MB (the scanout surfaces); LOAD_ASD, LOAD_TA/UNLOAD_TA (Navi 10's; the BC-250's SOS has
    //    none), LOAD_IP_FW (Navi 10's images; the cyan_skillfish2 ones come with the firmware stage) and 0xD are
    //    logged and reported done without being sent;
    //  - control commands on C2PMSG_64: ENABLE_INT (0x50000) and 0xB0000, which Linux does not send, are dropped;
    //    Apple's ring destroy 0xC0000 (DESTROY_GPCOM_RING, Linux's SR-IOV one) is sent as Linux's bare-metal
    //    DESTROY_RINGS (0x30000) and waited for (bit 31).
    constexpr UInt32 kPspC2PMsg64 = 0x80, kPspCtrlEnableInt = 0x50000, kPspCtrlUnknownB = 0xB0000;
    constexpr UInt32 kPspCtrlDestroyGpcom = 0xC0000, kPspCtrlDestroyRings = 0x30000, kPspResponseFlag = 0x80000000;
    constexpr UInt32 kPspCmdSetupTmr = 5, kPspCmdDestroyTmr = 7;
    constexpr UInt32 kPspTocSize = 8, kPspTocImage = 10;    // hw_init input dwords.
    constexpr UInt64 kPspTmrScanoutGuard = 64ULL << 20;

    bool pspLive = false;
    bool pspRingsDestroyed = false, pspTmrDestroyed = false;    // Linux's teardown completed.

    UInt32 wrapPspRead(void* psp, UInt32 reg, UInt32 baseIndex, UInt32 ip);

    UInt32 pspReadRaw(void* psp, UInt32 reg) { return FunctionCast(wrapPspRead, orgPspRead)(psp, reg, 0, 0x4B); }

    void wrapPspWrite(void* psp, UInt32 reg, UInt32 baseIndex, UInt32 value, UInt32 ip)
    {
        const bool log = ++pspWrites <= kPspWriteLogMax;
        if (!pspLive) {
            if (log) {
                BCLOG("BC250HWL", "PSP dry: W reg 0x%X (base %u) = 0x%08X (ip 0x%02X)", reg, baseIndex, value, ip);
            }
            return;
        }
        if (reg == kPspC2PMsg64 && baseIndex == 0) {
            if (value == kPspCtrlEnableInt || value == kPspCtrlUnknownB) {
                BCLOG("BC250HWL", "PSP: control 0x%X to C2PMSG_64 dropped (Linux does not send it)", value);
                return;
            }
            if (value == kPspCtrlDestroyGpcom) {
                FunctionCast(wrapPspWrite, orgPspWrite)(psp, reg, baseIndex, kPspCtrlDestroyRings, ip);
                UInt32 status = 0;
                for (UInt32 i = 0; i < 2000; i++) {
                    status = pspReadRaw(psp, kPspC2PMsg64);
                    if ((status & kPspResponseFlag) != 0) { break; }
                    IODelay(500);
                }
                pspRingsDestroyed = (status & kPspResponseFlag) != 0 && (status & 0xFFFF) == 0;
                BCLOG("BC250HWL", "PSP: ring destroy sent as DESTROY_RINGS (Apple's 0x%X): C2PMSG_64 0x%08X", value,
                    status);
                return;
            }
        }
        if (log) { BCLOG("BC250HWL", "PSP: W reg 0x%X (base %u) = 0x%08X (ip 0x%02X)", reg, baseIndex, value, ip); }
        FunctionCast(wrapPspWrite, orgPspWrite)(psp, reg, baseIndex, value, ip);
    }

    UInt32 wrapPspRead(void* psp, UInt32 reg, UInt32 baseIndex, UInt32 ip)
    {
        const auto value = FunctionCast(wrapPspRead, orgPspRead)(psp, reg, baseIndex, ip);
        if (++pspReads <= kPspReadLogMax) {
            BCLOG("BC250HWL", "PSP%s: R reg 0x%X (base %u) -> 0x%08X (ip 0x%02X)", pspLive ? "" : " dry", reg,
                baseIndex, value, ip);
        }
        return value;
    }

    UInt32 wrapPspRingCreate(void* psp, UInt32 type)
    {
        if (!pspLive) {
            BCLOG("BC250HWL", "PSP dry: ring create (type %u) not made, reported done", type);
            return 0;
        }
        const auto ret = FunctionCast(wrapPspRingCreate, orgPspRingCreate)(psp, type);
        BCLOG("BC250HWL", "PSP: ring create (type %u) <<< 0x%X, C2PMSG_64 0x%08X", type, ret,
            pspReadRaw(psp, kPspC2PMsg64));
        return ret;
    }

    // Level 17: the BC-250's own microcode through PSP, as Linux loads it on Cyan Skillfish2 (fw_load_type PSP).
    // PSP's hw_init input carries the firmware list TTL gathered from GC, SDMA and MES (+4 count, +8 list of 0x28-byte
    // entries: +4 type, +0x10 data, +0x18 size); Apple's Navi 10 images come from constants in HWLibs (GC: 0xcb83b68,
    // chosen in 0xc312f38). Apple's type numbers are its own (it loads its own SOS on Navi 10), not the ABI of the
    // SOS the BC-250's firmware preloads (Linux's psp_gfx_if.h). On the board Apple lists ME 4, CE 2, PFP 3, MEC 7,
    // RLC 11, SDMA0/1 12/13, RLC restore lists 25/23/24, RLC P 22, LX6 IRAM/DRAM 26/27 and two more (9/10). The images
    // match cyan_skillfish2's in layout (same sizes; MEC split at its jump table, 0x414B0 + 0x380) and nearly in
    // content (the signatures differ). The list is rebuilt on the input copy: each image Linux loads on the BC-250
    // gets the cyan_skillfish2 payload (ucode_array_offset/ucode_size, MEC without and with only its jump table), the
    // jump tables and MEC2 are added under private types, everything else is dropped; each LOAD_IP_FW is then sent
    // with the ABI type. Versions on the BC-250 under Linux: ME 0x63, PFP 0x94, CE 0x25, RLC 0xd, MEC/MEC2 0x90, SDMA
    // 0x34. Firmware: linux-firmware amdgpu/cyan_skillfish2_*.bin (Firmware/LICENSE.amdgpu).
    const UInt8 kCs2Me[] = {
#embed "Firmware/cyan_skillfish2_me.bin"
    };
    const UInt8 kCs2Pfp[] = {
#embed "Firmware/cyan_skillfish2_pfp.bin"
    };
    const UInt8 kCs2Ce[] = {
#embed "Firmware/cyan_skillfish2_ce.bin"
    };
    const UInt8 kCs2Mec[] = {
#embed "Firmware/cyan_skillfish2_mec.bin"
    };
    const UInt8 kCs2Mec2[] = {
#embed "Firmware/cyan_skillfish2_mec2.bin"
    };
    const UInt8 kCs2Rlc[] = {
#embed "Firmware/cyan_skillfish2_rlc.bin"
    };
    const UInt8 kCs2Sdma0[] = {
#embed "Firmware/cyan_skillfish2_sdma.bin"
    };
    const UInt8 kCs2Sdma1[] = {
#embed "Firmware/cyan_skillfish2_sdma1.bin"
    };

    enum class FwPart
    {
        All,       // ucode_size bytes at ucode_array_offset.
        NoJt,      // The same without the jump table (Linux's CP_MEC1/MEC2).
        JtOnly,    // Only the jump table (jt_offset/jt_size, dwords; Linux's CP_MEC1_JT/MEC2_JT).
    };

    struct PspFwImage
    {
        UInt32       appleType, abiType;
        const char*  name;
        const UInt8* file;
        size_t       fileSize;
        FwPart       part;
    };
    constexpr UInt32 kAppleTypeMec = 7, kAppleTypeMecJt = 0x30, kAppleTypeMec2 = 0x31, kAppleTypeMec2Jt = 0x32;
    // In Linux's load order (psp_load_non_psp_fw: SDMA, CE, PFP, ME, MEC and JT, MEC2 and JT, RLC_G last) with
    // psp_get_fw_type's ABI types (MEC2 is CP_MEC again, its JT CP_MEC_ME2); the command carries the Apple type that
    // Apple's table turns into it (pspAppleCmdType).
    const PspFwImage kPspFwImages[] = {
        {12, 9, "SDMA0", kCs2Sdma0, sizeof(kCs2Sdma0), FwPart::All},
        {13, 10, "SDMA1", kCs2Sdma1, sizeof(kCs2Sdma1), FwPart::All},
        {2, 3, "CE", kCs2Ce, sizeof(kCs2Ce), FwPart::All},
        {3, 2, "PFP", kCs2Pfp, sizeof(kCs2Pfp), FwPart::All},
        {4, 1, "ME", kCs2Me, sizeof(kCs2Me), FwPart::All},
        {kAppleTypeMec, 4, "MEC", kCs2Mec, sizeof(kCs2Mec), FwPart::NoJt},
        {kAppleTypeMecJt, 5, "MEC JT", kCs2Mec, sizeof(kCs2Mec), FwPart::JtOnly},
        {kAppleTypeMec2, 4, "MEC2", kCs2Mec2, sizeof(kCs2Mec2), FwPart::NoJt},
        {kAppleTypeMec2Jt, 6, "MEC2 JT", kCs2Mec2, sizeof(kCs2Mec2), FwPart::JtOnly},
        {11, 8, "RLC_G", kCs2Rlc, sizeof(kCs2Rlc), FwPart::All},
    };
    constexpr UInt32 kPspCmdLoadIpFw = 6, kPspFwEntrySize = 0x28, kPspFwEntryType = 4, kPspFwEntryData = 0x10;
    constexpr UInt32 kPspFwEntryLength = 0x18, kPspFwListMax = 24;
    constexpr UInt32 kPspFwCount = 1, kPspFwList = 2;    // hw_init input dwords.

    bool   fwLive = false;
    constexpr UInt32 kPspFlags = 0x28;
    constexpr UInt8  kPspFlagPerImageBuffers = 1;
    UInt8  pspFwList[kPspFwListMax][kPspFwEntrySize] {};
    UInt32 fwLoaded = 0, fwFailed = 0;

    // The inverse of Apple's command type table for the ABI types loaded here.
    constexpr UInt32 pspAppleCmdType(UInt32 abiType)
    {
        switch (abiType) {
            case 1: return 4;      // CP_ME
            case 2: return 3;      // CP_PFP
            case 3: return 2;      // CP_CE
            case 4: return 7;      // CP_MEC
            case 5: return 5;      // CP_MEC_ME1 (MEC JT)
            case 6: return 6;      // CP_MEC_ME2 (MEC2 JT)
            case 8: return 11;     // RLC_G
            case 9: return 12;     // SDMA0
            case 10: return 13;    // SDMA1
            default: return 0;     // Translates to 0: refused by the SOS.
        }
    }

    const PspFwImage* pspFwImage(UInt32 appleType)
    {
        for (const auto& image : kPspFwImages) {
            if (image.appleType == appleType) { return &image; }
        }
        return nullptr;
    }

    // The payload as Linux's amdgpu_ucode prepares it for PSP (common_firmware_header, then gfx_firmware_header_v1_0's
    // jt_offset/jt_size at +0x24/+0x28).
    bool pspFwPayload(const PspFwImage& image, const UInt8** data, UInt32* size)
    {
        if (image.fileSize < 0x2C) { return false; }
        const auto* file   = image.file;
        const auto  usize  = *reinterpret_cast<const UInt32*>(file + 0x14);
        const auto  offset = *reinterpret_cast<const UInt32*>(file + 0x18);
        if (offset >= image.fileSize || usize > image.fileSize - offset) { return false; }
        const auto jtOffset = *reinterpret_cast<const UInt32*>(file + 0x24) * 4;
        const auto jtSize   = *reinterpret_cast<const UInt32*>(file + 0x28) * 4;
        switch (image.part) {
            case FwPart::All: *data = file + offset, *size = usize; return true;
            case FwPart::NoJt:
                if (jtSize == 0 || jtSize >= usize) { return false; }
                *data = file + offset, *size = usize - jtSize;
                return true;
            case FwPart::JtOnly:
                if (jtSize == 0 || jtOffset > usize || jtSize > usize - jtOffset) { return false; }
                *data = file + offset + jtOffset, *size = jtSize;
                return true;
        }
        return false;
    }

    bool pspFwSetEntry(UInt8* entry, const PspFwImage& image)
    {
        const UInt8* data = nullptr;
        UInt32       size = 0;
        if (!pspFwPayload(image, &data, &size)) {
            BCLOG("BC250HWL", "PSP firmware: cyan_skillfish2 %s has an unexpected header; not loaded", image.name);
            return false;
        }
        getMember<UInt32>(entry, kPspFwEntryType)        = image.appleType;
        getMember<const UInt8*>(entry, kPspFwEntryData)  = data;
        getMember<UInt32>(entry, kPspFwEntryLength)       = size;
        BCLOG("BC250HWL", "PSP firmware: %s = cyan_skillfish2, 0x%X bytes, version 0x%X (list type 0x%X, PSP type %u)",
            image.name, size, *reinterpret_cast<const UInt32*>(image.file + 0x10), image.appleType, image.abiType);
        return true;
    }

    // Rebuilds the firmware list of PSP's hw_init input (in the copy) for the BC-250; returns the new count.
    UInt32 pspFwRebuildList(const UInt8* list, UInt32 count)
    {
        const UInt8* mec = nullptr;
        for (UInt32 i = 0; i < count; i++) {
            const auto* entry = list + i * kPspFwEntrySize;
            const auto  type  = getMember<UInt32>(const_cast<UInt8*>(entry), kPspFwEntryType);
            if (type == kAppleTypeMec) { mec = entry; }
            if (pspFwImage(type) == nullptr) {
                BCLOG("BC250HWL", "PSP firmware: list type 0x%X (Navi 10, 0x%X bytes) dropped: not loaded on the BC-250",
                    type, getMember<UInt32>(const_cast<UInt8*>(entry), kPspFwEntryLength));
            }
        }
        // Each image takes Apple's entry of its type as the template (the jump tables and MEC2 take MEC's).
        UInt32 out = 0;
        for (const auto& image : kPspFwImages) {
            const UInt8* entry = nullptr;
            for (UInt32 i = 0; i < count && entry == nullptr; i++) {
                const auto* e = list + i * kPspFwEntrySize;
                if (getMember<UInt32>(const_cast<UInt8*>(e), kPspFwEntryType) == image.appleType) { entry = e; }
            }
            if (entry == nullptr && image.appleType >= kAppleTypeMecJt) { entry = mec; }
            if (entry == nullptr || out >= kPspFwListMax) { continue; }
            memcpy(pspFwList[out], entry, kPspFwEntrySize);
            if (pspFwSetEntry(pspFwList[out], image)) { out++; }
        }
        return out;
    }

    // Level 17: the TMR naturally aligned (its size, 4 MB), as AMD's hardware engineers prefer (amdgpu_psp.c) and as
    // Linux's lands on the BC-250 (0xF41F800000); Apple's TMR init (0xc358b19) asks for 1 MB alignment, which put it at
    // 0xF41FB00000. The immediate of "mov dword [rbx + 0xB00], 0x100000" after the 4 MB size is patched.
    const UInt8 kPspTmrAlignOriginal[] = {0x48, 0xC7, 0x83, 0xF8, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xC7, 0x83,
        0x00, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00};
    const UInt8 kPspTmrAlignPatched[]  = {0x48, 0xC7, 0x83, 0xF8, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xC7, 0x83,
         0x00, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00};
    static_assert(sizeof(kPspTmrAlignOriginal) == sizeof(kPspTmrAlignPatched));

    bool pspCommandAllowed(const UInt32* cmd)
    {
        if (cmd[0] == kPspCmdLoadIpFw) { return fwLive && pspFwImage(cmd[4]) != nullptr; }
        if (cmd[0] == kPspCmdDestroyTmr) { return true; }
        if (cmd[0] != kPspCmdSetupTmr) { return false; }
        const UInt64 address = (static_cast<UInt64>(cmd[2]) << 32) | cmd[1];
        const UInt64 size    = cmd[3];
        if (size == 0 || address < gvmFbBase + kPspTmrScanoutGuard || address + size > gvmFbBase + gvmFbSize) {
            BCLOG("BC250HWL", "PSP: SETUP_TMR at 0x%llX, 0x%llX bytes refused (FB 0x%llX, 0x%llX bytes)", address,
                size, gvmFbBase, gvmFbSize);
            return false;
        }
        return true;
    }

    // Apple's command layout: +0 cmd_id, +4/+8 buffer MC address, +0xC size, +0x10 firmware type (LOAD_IP_FW).
    UInt32 wrapPspCmdSubmit(void* psp, const UInt32* cmd, UInt32* response, UInt32* handle)
    {
        if (cmd == nullptr) { return FunctionCast(wrapPspCmdSubmit, orgPspCmdSubmit)(psp, cmd, response, handle); }
        if (pspLive && pspCommandAllowed(cmd)) {
            if (cmd[0] == kPspCmdLoadIpFw) {
                // psp_cmd_km_buf_prep translates the command's type through Apple's table (c358aec: 2->3 CE, 3->2 PFP,
                // 4->1 ME, 5->5, 6->6, 7->4 MEC, 11->8 RLC_G, 12->9 SDMA0, 13->10 SDMA1), so it is given the Apple
                // type that becomes the image's ABI type (the command is Apple's own buffer).
                const auto* image = pspFwImage(cmd[4]);
                const_cast<UInt32*>(cmd)[4] = pspAppleCmdType(image->abiType);
                const auto ret = FunctionCast(wrapPspCmdSubmit, orgPspCmdSubmit)(psp, cmd, response, handle);
                // With a handle (per-image buffers) the command is only queued: the SOS's status is not known here.
                const bool ok  = ret == 0;
                (ok ? fwLoaded : fwFailed)++;
                if (handle != nullptr) {
                    BCLOG("BC250HWL", "PSP: LOAD_IP_FW %s (type %u, sent as Apple's %u, 0x%X bytes at 0x%X%08X) queued "
                                      "<<< 0x%X, handle %u",
                        image->name, image->abiType, cmd[4], cmd[3], cmd[2], cmd[1], ret, *handle);
                } else {
                    BCLOG("BC250HWL", "PSP: LOAD_IP_FW %s (type %u, sent as Apple's %u, 0x%X bytes at 0x%X%08X) <<< 0x%X, "
                                      "status %08X",
                        image->name, image->abiType, cmd[4], cmd[3], cmd[2], cmd[1], ret, response ? response[0] : 0);
                }
                return ret;
            }
            if (cmd[0] == kPspCmdSetupTmr && fwLive) {
                // Level 17: SETUP_TMR as Linux's psp_prep_tmr_cmd_buf builds it: besides the TMR's GPU address, its
                // system physical address (the BC-250's FB is a carve-out at GCMC_VM_FB_OFFSET) with virt_phy_addr
                // set (+0x10 flags bit 1, +0x14/+0x18 the address). Apple sends only the GPU address; with it the SOS
                // took the first images and refused later ones inconsistently (MEC 0xFFFF300F, SDMA0 0xFFFF0006).
                auto*        mutableCmd = const_cast<UInt32*>(cmd);
                const UInt64 gpu        = (static_cast<UInt64>(cmd[2]) << 32) | cmd[1];
                const UInt64 phys       = gpu - gvmFbBase + gvmFbOffset;
                mutableCmd[4]           = 1U << 1;
                mutableCmd[5]           = static_cast<UInt32>(phys);
                mutableCmd[6]           = static_cast<UInt32>(phys >> 32);
                BCLOG("BC250HWL", "PSP: SETUP_TMR with system physical address 0x%llX (GPU 0x%llX)", phys, gpu);
            }
            const auto ret = FunctionCast(wrapPspCmdSubmit, orgPspCmdSubmit)(psp, cmd, response, handle);
            if (cmd[0] == kPspCmdDestroyTmr) { pspTmrDestroyed = ret == 0 && (response == nullptr || response[0] == 0); }
            BCLOG("BC250HWL", "PSP: command %u (%08X %08X %08X %08X) submitted <<< 0x%X, response %08X %08X %08X %08X",
                cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], ret, response ? response[0] : 0, response ? response[1] : 0,
                response ? response[2] : 0, response ? response[3] : 0);
            return ret;
        }
        if (++pspCmds <= kPspCmdLogMax) {
            BCLOG("BC250HWL", "PSP%s: command %u not submitted, reported done: %08X %08X %08X %08X %08X %08X %08X %08X",
                pspLive ? "" : " dry", cmd[0], cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6], cmd[7]);
        }
        if (handle != nullptr) { *handle = 0; }
        return 0;
    }

    UInt32 wrapPspHwInit(void* psp, const UInt32* input, void* output)
    {
        UInt32 copy[64] {};
        if (input != nullptr) {
            for (UInt32 i = 0; i < kPspInputDwords; i += 4) {
                BCLOG("BC250HWL", "PSP hw_init input +0x%02X: %08X %08X %08X %08X", i * 4, input[i], input[i + 1],
                    input[i + 2], input[i + 3]);
            }
            // Level 16: the same input without the TOC (the input's size is its first dword).
            const UInt32 bytes = input[0];
            if (pspLive && bytes >= (kPspTocImage + 2) * sizeof(UInt32) && bytes <= sizeof(copy)) {
                memcpy(copy, input, bytes);
                copy[kPspTocSize]  = 0;
                copy[kPspTocImage] = copy[kPspTocImage + 1] = 0;
                input              = copy;
                BCLOG("BC250HWL", "PSP hw_init: TOC removed from the input (Linux loads none on PSP 11.0.8)");
                // Level 17: the firmware list with the BC-250's images.
                const auto* list = *reinterpret_cast<const UInt8* const*>(&copy[kPspFwList]);
                if (fwLive && list != nullptr && copy[kPspFwCount] <= kPspFwListMax) {
                    const auto count = pspFwRebuildList(list, copy[kPspFwCount]);
                    BCLOG("BC250HWL", "PSP firmware: list of %u Navi 10 image(s) rebuilt as %u cyan_skillfish2 image(s)",
                        copy[kPspFwCount], count);
                    copy[kPspFwCount]                                  = count;
                    *reinterpret_cast<const UInt8**>(&copy[kPspFwList]) = pspFwList[0];
                }
            }
        }
        // Level 17: each image in its own page-aligned staging buffer (flag bit 0 of psp+0x28, which PSP's sw_init sets
        // from an OS-services query; psp_np_fw_load then uses the per-image buffers c358fa8 allocates: size + 4 KB,
        // 4 KB aligned, memory type 3), as Linux gives each ucode its own page in its fw buffer, instead of one shared
        // 1 MB VRAM buffer. With the shared buffer the SOS took RLC_G, ME, CE and PFP and refused MEC (0xFFFF300F) and
        // SDMA0 (0xFFFF0006) whatever the order and TMR.
        if (fwLive && psp != nullptr) {
            getMember<UInt8>(psp, kPspFlags) |= kPspFlagPerImageBuffers;
            BCLOG("BC250HWL", "PSP firmware: per-image staging buffers (flags 0x%02X)", getMember<UInt8>(psp, kPspFlags));
        }
        const auto ret = FunctionCast(wrapPspHwInit, orgPspHwInit)(psp, input, output);
        BCLOG("BC250HWL", "PSP hw_init (%s) <<< 0x%X after %u write(s), %u read(s), %u command(s) not submitted, "
                          "%u firmware image(s) loaded, %u failed",
            pspLive ? "live" : "dry run", ret, pspWrites, pspReads, pspCmds, fwLoaded, fwFailed);
        return ret;
    }

    UInt32 wrapPspHwFini(void* psp)
    {
        const auto ret = FunctionCast(wrapPspHwFini, orgPspHwFini)(psp);
        BCLOG("BC250HWL", "PSP hw_fini (%s) <<< 0x%X", pspLive ? "live" : "dry run", ret);
        // Apple's cleanup also stops and frees a UM ring (type 1: control 0xB0000, dropped here) that its hw_init never
        // created, which fails on its own. Once the TMR is destroyed and Linux's DESTROY_RINGS is acknowledged, the
        // PSP is left as Linux leaves it: report success.
        if (pspLive && ret != 0 && pspTmrDestroyed && pspRingsDestroyed) {
            BCLOG("BC250HWL", "PSP hw_fini: TMR destroyed and rings destroyed as Linux does; reported done");
            return 0;
        }
        return ret;
    }

    // Returns false if a write or submit path could not be hooked; PSP's hw_init must then not run.
    bool hookPspDryRun(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        struct Hook
        {
            const char*        name;
            const UInt8*       pattern;
            size_t             length;
            mach_vm_address_t  wrapper;
            mach_vm_address_t& org;
            bool               guard;
        };
        Hook hooks[] = {
            {"register write", kPspWritePattern, sizeof(kPspWritePattern),
                reinterpret_cast<mach_vm_address_t>(wrapPspWrite), orgPspWrite, true},
            {"ring create", kPspRingCreatePattern, sizeof(kPspRingCreatePattern),
                reinterpret_cast<mach_vm_address_t>(wrapPspRingCreate), orgPspRingCreate, true},
            {"command submit", kPspCmdSubmitPattern, sizeof(kPspCmdSubmitPattern),
                reinterpret_cast<mach_vm_address_t>(wrapPspCmdSubmit), orgPspCmdSubmit, true},
            {"register read", kPspReadPattern, sizeof(kPspReadPattern),
                reinterpret_cast<mach_vm_address_t>(wrapPspRead),
                orgPspRead, false},
            {"hw_init", kPspHwInitPattern, sizeof(kPspHwInitPattern),
                reinterpret_cast<mach_vm_address_t>(wrapPspHwInit),
                orgPspHwInit, false},
            {"hw_fini", kPspHwFiniPattern, sizeof(kPspHwFiniPattern),
                reinterpret_cast<mach_vm_address_t>(wrapPspHwFini),
                orgPspHwFini, false},
        };
        mach_vm_address_t at[arrsize(hooks)] {};
        for (size_t i = 0; i < arrsize(hooks); i++) {
            at[i] = findUnique(hooks[i].pattern, nullptr, hooks[i].length, slide, size);
            if (at[i] == 0) {
                BCLOG("BC250HWL", "level 15: PSP %s not found", hooks[i].name);
                if (hooks[i].guard) { return false; }
            }
        }
        for (size_t i = 0; i < arrsize(hooks); i++) {
            if (at[i] == 0) { continue; }
            KernelPatcher::RouteRequest request {nullptr, hooks[i].wrapper, hooks[i].org};
            request.from = at[i];
            if (!patcher.routeMultiple(id, &request, 1)) {
                BCLOG("BC250HWL", "level 15: PSP %s failed to route", hooks[i].name);
                patcher.clearError();
                if (hooks[i].guard) { return false; }
            }
        }
        return true;
    }

    // Level 18: GC's hw_init as a dry run. gc.c's hw_init (0xc30e03e) runs GC 10.1's per-version tables (golden
    // settings, RLC/CP/GFX setup through gc_10_1_ip*.c), too large to check by reading. GC reaches the hardware only
    // through its CGS wrappers (gc_cgs.c): write (0xc31143a, +0x38), write_ext (0xc3113fc, +0x100), write_ext2
    // (0xc311475, +0x120), read (0xc3114b3, +0x40), read_ext2 (0xc3114f4, +0x118), and the golden-settings query
    // (0xc311598, +0x90). At this level every write is logged and not made, reads and the golden query run and are
    // logged, so the log is the list of what GC would program on the BC-250, to be checked against Linux's gfx_v10_0
    // (Cyan Skillfish: golden_settings_gc_10_0_cyan_skillfish, RLC_PG_CNTL bit 23, no GFXOFF, CU harvest masks).
    const UInt8 kGcWritePattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x47, 0x08, 0x49, 0x8B, 0x40, 0x38, 0x48,
            0x85, 0xC0, 0x74, 0x07, 0x49, 0x8B, 0x78, 0x08, 0x5D, 0xFF, 0xE0, 0x48, 0x8D, 0x15, 0x8C};
    const UInt8 kGcWriteExtPattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x47, 0x08, 0x49, 0x8B, 0x80, 0x00, 0x01,
         0x00, 0x00, 0x48};
    const UInt8 kGcWriteExt2Pattern[] = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x4F, 0x08, 0x49, 0x8B, 0x81, 0x20, 0x01,
        0x00, 0x00, 0x48};
    const UInt8 kGcReadPattern[]      = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x4F, 0x08, 0x48, 0x8B, 0x41, 0x40, 0x48,
             0x85, 0xC0, 0x74, 0x07, 0x48, 0x8B, 0x79, 0x08, 0x5D, 0xFF, 0xE0, 0x48, 0x8D, 0x15, 0x44};
    const UInt8 kGcReadExt2Pattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x47, 0x08, 0x49, 0x8B, 0x80, 0x18, 0x01,
         0x00, 0x00, 0x48};
    const UInt8 kGcGoldenPattern[]    = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x89, 0x75, 0xE8, 0x89,
           0x55, 0xEC, 0x89};
    const UInt8 kGcHwInitPattern[]    = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83,
           0xEC, 0x20, 0x48, 0x8B};
    constexpr UInt32 kGcWriteLogMax = 4000, kGcReadLogMax = 2500;
    UInt32           gcSpmWrites = 0;

    mach_vm_address_t orgGcWrite = 0, orgGcWriteExt = 0, orgGcWriteExt2 = 0, orgGcRead = 0, orgGcReadExt2 = 0,
                      orgGcGolden = 0, orgGcHwInit = 0;
    UInt32 gcWrites = 0, gcReads = 0;

    mach_vm_address_t gcCaller(void* address)
    {
        mach_vm_address_t offset = 0;
        kextOf(reinterpret_cast<mach_vm_address_t>(address), &offset);
        return offset;
    }

    bool gcIsSpmDelay(UInt64 reg)
    {
        // RLC_SPM_GLB/SE_SAMPLEDELAY_IND_ADDR/DATA (GC base 1).
        constexpr UInt64 kSpmDelayRegs[] = {0xA000 + 0x3C90, 0xA000 + 0x3C91, 0xA000 + 0x3C92, 0xA000 + 0x3C93};
        for (const auto r : kSpmDelayRegs) {
            if (reg == r) { return true; }
        }
        return false;
    }

    // Arguments after the GC context are logged as passed (register, value, block/instance for the plain ones).
    // Level 19: GC's writes are made (gcLive), except Navi 10's SPM sample delays (RLC_SPM_*_SAMPLEDELAY_IND_ADDR/DATA,
    // hundreds of pairs), which Linux does not program on Cyan Skillfish (init_spm_golden covers Navi 10/12/14 only):
    // those are counted and dropped at every level.
    bool gcLive = false;
    bool gcRlcStartPending = false;
    void gcRlcStart(void* gc);
    void gcCpSnapshot(void* gc);
    void gcCpBrief(const char* when);
    void pspRingStatusDump();
    const UInt8 kKiqSetResourcesOriginal[] = {0x48, 0xB9, 0x00, 0xA0, 0x06, 0xC0, 0xFF, 0xFF, 0x28, 0x00};
    const UInt8 kKiqSetResourcesPatched[]  = {0x48, 0xB9, 0x00, 0xA0, 0x06, 0xC0, 0x00, 0x00, 0x00, 0x00};
    constexpr UInt32 kCpMecCntl = 0x1260 + 0xE2D, kCpMecHalt = 0x50000000;
    constexpr UInt32 kCpPqStatus = 0x1260 + 0x1E58;    // DOORBELL_ENABLE: the last write of Apple's KIQ init.

    template<mach_vm_address_t* Org>
    UInt64 gcWriteCommon(const char* kind, void* gc, UInt64 a, UInt64 b, UInt64 c, UInt64 d, void* caller)
    {
        if (gcIsSpmDelay(a)) {
            gcSpmWrites++;
            return 0;
        }
        if (++gcWrites <= kGcWriteLogMax) {
            BCLOG("BC250HWL", "GC%s: W%s (0x%llX, 0x%llX, 0x%llX, 0x%llX) from +0x%llX", gcLive ? "" : " dry", kind, a, b,
                c, d, gcCaller(caller));
        }
        // Level 27: the PM4 engine's start halts the MEC (CP_MEC_CNTL MEC_ME1/ME2_HALT) when its KIQ does not answer;
        // the CP's state is captured first.
        if (gcLive && a == kCpMecCntl && (b & kCpMecHalt) == kCpMecHalt) { gcCpSnapshot(gc); }
        // The CP before the KIQ's MEC is unhalted and once its queue is enabled, before SET_RESOURCES: shows whether
        // the fetcher (CPF, shared by GFX and compute) is already busy, e.g. on the GFX ring.
        if (gcLive && a == kCpMecCntl && b == 0) {
            gcCpBrief("before MEC unhalt");
            pspRingStatusDump();
        }
        UInt64 ret = 0;
        if (gcLive) { ret = reinterpret_cast<UInt64 (*)(void*, UInt64, UInt64, UInt64, UInt64)>(*Org)(gc, a, b, c, d); }
        if (gcLive && a == kCpPqStatus) {
            IODelay(1000);
            gcCpBrief("KIQ enabled, 1 ms");
        }
        // Linux's gfx_v10_0_rlc_resume (PSP, no autoload) starts the RLC right after init_csb (RLC_CSIB_ADDR_HI/LO,
        // RLC_CSIB_LENGTH); Apple programs the clear-state buffer later than the autoload wait, so the start waits
        // for its RLC_CSIB_LENGTH write.
        if (gcRlcStartPending && a == 0xA000 + 0x4CA4) {
            gcRlcStartPending = false;
            gcRlcStart(gc);
        }
        return ret;
    }

    UInt64 wrapGcWrite(void* gc, UInt64 a, UInt64 b, UInt64 c, UInt64 d)
    {
        return gcWriteCommon<&orgGcWrite>("", gc, a, b, c, d, __builtin_return_address(0));
    }

    UInt64 wrapGcWriteExt(void* gc, UInt64 a, UInt64 b, UInt64 c, UInt64 d)
    {
        return gcWriteCommon<&orgGcWriteExt>(" ext", gc, a, b, c, d, __builtin_return_address(0));
    }

    UInt64 wrapGcWriteExt2(void* gc, UInt64 a, UInt64 b, UInt64 c, UInt64 d)
    {
        return gcWriteCommon<&orgGcWriteExt2>(" ext2", gc, a, b, c, d, __builtin_return_address(0));
    }

    UInt32 wrapGcRead(void* gc, UInt64 a, UInt64 b, UInt64 c)
    {
        const auto value = FunctionCast(wrapGcRead, orgGcRead)(gc, a, b, c);
        if (++gcReads <= kGcReadLogMax) {
            mach_vm_address_t from = 0;
            kextOf(reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0)), &from);
            BCLOG("BC250HWL", "GC dry: R 0x%05llX -> 0x%08X (0x%llX, 0x%llX) from +0x%llX", a, value, b, c, from);
        }
        return value;
    }

    UInt32 wrapGcReadExt2(void* gc, UInt64 a, UInt64 b, UInt64 c, UInt64 d)
    {
        const auto value = FunctionCast(wrapGcReadExt2, orgGcReadExt2)(gc, a, b, c, d);
        if (++gcReads <= kGcReadLogMax) {
            mach_vm_address_t from = 0;
            kextOf(reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0)), &from);
            BCLOG("BC250HWL", "GC dry: R ext2 (0x%llX, 0x%llX, 0x%llX, 0x%llX) -> 0x%08X from +0x%llX", a, b, c, d, value,
                from);
        }
        return value;
    }

    // Linux's golden settings for Cyan Skillfish (gfx_v10_0.c golden_settings_gc_10_0_cyan_skillfish, sdma_v5_0.c
    // golden_settings_sdma_cyan_skillfish) in Apple's format ({offset, base index, mask, value}, ended by base index
    // 0xFFFFFFFF), in place of Apple's GC 10.1 table (Navi 10's, which also carries SDMA's). On the board Apple's
    // differed in GB_ADDR_CONFIG (0x100044, Linux 0x44), pipe steering, GL2C_CTRL2/3, DB_DEBUG4, DB_LAST_OF_BURST_CONFIG,
    // GCR_GENERAL_CNTL, CB_HW_CONTROL_4 and lacked GE_FAST_CLKS, CH_*, GL1_DRAM_BURST_CTRL, BINNER_EVENT_CNTL_0 and others.
    struct GoldenEntry
    {
        UInt32 offset, baseIndex, mask, value;
    };
    const GoldenEntry kGcGoldenCyanSkillfish[] = {
        {0x2200, 1, 0xffffffff, 0xe0000000},    // GRBM_GFX_INDEX
        {0xfe8, 0, 0x3fffffff, 0x0000493e},    // GE_FAST_CLKS
        {0x50b1, 1, 0xfcff8fff, 0xf8000100},    // CGTT_CPF_CLK_CTRL
        {0x5080, 1, 0xff7f0fff, 0x3c000100},    // CGTT_SPI_CLK_CTRL
        {0x1423, 0, 0xa0000000, 0xa0000000},    // CB_HW_CONTROL_3
        {0x1422, 0, 0x00008000, 0x003c8014},    // CB_HW_CONTROL_4
        {0x2d84, 1, 0x00000010, 0x00000017},    // CH_DRAM_BURST_CTRL
        {0x2d90, 1, 0xffffffff, 0xd8d8d8d8},    // CH_PIPE_STEER
        {0x2d94, 1, 0x00000003, 0x00000003},    // CH_VC5_ENABLE
        {0x1f57, 0, 0x800007ff, 0x000005ff},    // CP_SD_CNTL
        {0x13ac, 0, 0xffffffff, 0x20000000},    // DB_DEBUG
        {0x13ae, 0, 0xffffffff, 0x00000200},    // DB_DEBUG3
        {0x13af, 0, 0xffffffff, 0x04800000},    // DB_DEBUG4
        {0x13ba, 0, 0xffffffff, 0x03860210},    // DB_LAST_OF_BURST_CONFIG
        {0x13de, 0, 0x0c1800ff, 0x00000044},    // GB_ADDR_CONFIG
        {0x1583, 0, 0x00009d00, 0x00008500},    // GCR_GENERAL_CNTL
        {0x1712, 0, 0xffffffff, 0x000fffff},    // GCMC_VM_CACHEABLE_DRAM_ADDRESS_END
        {0x2d04, 1, 0x00000010, 0x00000017},    // GL1_DRAM_BURST_CTRL
        {0x2d10, 1, 0xfcfcfcfc, 0xd8d8d8d8},    // GL1_PIPE_STEER
        {0x2e25, 1, 0x77707770, 0x21302130},    // GL2_PIPE_STEER_0
        {0x2e26, 1, 0x77707770, 0x21302130},    // GL2_PIPE_STEER_1
        {0x2e21, 1, 0xffffffff, 0xffffffcf},    // GL2A_ADDR_MATCH_MASK
        {0x2e03, 1, 0xffffffff, 0xffffffcf},    // GL2C_ADDR_MATCH_MASK
        {0x50ac, 1, 0x10000000, 0x10000100},    // GL2C_CGTT_SCLK_CTRL
        {0x2e01, 1, 0xfc02002f, 0x9402002f},    // GL2C_CTRL2
        {0x2e0c, 1, 0x00002188, 0x00000188},    // GL2C_CTRL3
        {0x109c, 0, 0x08000009, 0x08000009},    // PA_SC_ENHANCE
        {0x106c, 0, 0xcc3fcc03, 0x842a4c02},    // PA_SC_BINNER_EVENT_CNTL_0
        {0x2281, 1, 0x0000000f, 0x00000000},    // PA_SC_LINE_STIPPLE_STATE
        {0x153f, 0, 0xffff3109, 0xffff3101},    // RMI_SPARE
        {0x10ac, 0, 0x00000100, 0x00000130},    // SQ_ARB_CONFIG
        {0x5090, 1, 0xffffffff, 0xffffffff},    // SQ_LDS_CLK_CTRL
        {0x12e2, 0, 0x00030008, 0x01030000},    // TA_CNTL_AUX
        {0x1588, 0, 0x00800000, 0x00800000},    // UTCL1_CTRL
        {0x1d, 0, 0xffbf1f0f, 0x03ab0107},    // SDMA0_CHICKEN_BITS
        {0x1e, 0, 0x001877ff, 0x00000044},    // SDMA0_GB_ADDR_CONFIG
        {0x1f, 0, 0x001877ff, 0x00000044},    // SDMA0_GB_ADDR_CONFIG_READ
        {0x87, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0xe7, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x147, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x1a7, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x207, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x267, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x2c7, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x327, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x387, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x3e7, 0, 0xfffffff7, 0x00403000},    // SDMA0_RB_WPTR_POLL_CNTL
        {0x48, 0, 0x007fffff, 0x004c5c00},    // SDMA0_UTCL1_PAGE
        {0x61d, 0, 0xffbf1f0f, 0x03ab0107},    // SDMA1_CHICKEN_BITS
        {0x61e, 0, 0x001877ff, 0x00000044},    // SDMA1_GB_ADDR_CONFIG
        {0x61f, 0, 0x001877ff, 0x00000044},    // SDMA1_GB_ADDR_CONFIG_READ
        {0x687, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x6e7, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x747, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x7a7, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x807, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x867, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x8c7, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x927, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x987, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x9e7, 0, 0xfffffff7, 0x00403000},    // SDMA1_RB_WPTR_POLL_CNTL
        {0x648, 0, 0x007fffff, 0x004c5c00},    // SDMA1_UTCL1_PAGE
        {0xFFFFFFFF, 0xFFFFFFFF, 0, 0},
    };

    void* wrapGcGolden(void* gc, UInt32 type, UInt32 major, UInt32 minor)
    {
        const auto table = FunctionCast(wrapGcGolden, orgGcGolden)(gc, type, major, minor);
        if (type == 0x0B && major == 10 && minor == 1 && table != nullptr) {
            BCLOG("BC250HWL", "GC golden settings for 10.1: Apple's (Navi 10) %p replaced by Linux's Cyan Skillfish list "
                              "(%zu entries)", table, arrsize(kGcGoldenCyanSkillfish) - 1);
            return const_cast<GoldenEntry*>(kGcGoldenCyanSkillfish);
        }
        BCLOG("BC250HWL", "GC golden settings for 0x%02X %u.%u: %p", type, major, minor, table);
        return table;
    }

    // Level 18: GC's check that PSP loaded its firmware (gc_check_ucode_loaded_by_psp 0xc31067b asks 0xc311606(gc,
    // GC firmware type) per image it has). The BC-250 loads what Linux loads on Cyan Skillfish2 (RLC_G, ME, CE, PFP,
    // MEC, MEC2 and their jump tables); Apple's Navi 10 set also has the RLC save/restore lists (0-2), RLC V (11),
    // RLC P (12), LX6 IRAM/DRAM (13, 14), TOC (15) and tap delays (16-20), which are not loaded: those are reported
    // as not needed. The answer for the others is PSP's.
    const UInt8 kGcFwLoadedPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x10, 0x31, 0xC0, 0x48, 0x89, 0x45,
        0xF8, 0x48, 0x89};
    mach_vm_address_t orgGcFwLoaded = 0;

    UInt8 wrapGcFwLoaded(void* gc, UInt32 type)
    {
        const auto loaded = FunctionCast(wrapGcFwLoaded, orgGcFwLoaded)(gc, type);
        const bool used   = type >= 3 && type <= 10;
        BCLOG("BC250HWL", "GC firmware %u loaded by PSP: %u%s", type, loaded,
            used ? "" : " (not used on the BC-250: reported loaded)");
        return used ? loaded : 1;
    }

    void gcUnlockCus(void* gc);

    UInt32 wrapGcHwInit(void* gc, void* input, void* output)
    {
        BCLOG("BC250HWL", "GC hw_init (%s) >>>", gcLive ? "live" : "dry run");
        if (gcLive) { gcUnlockCus(gc); }
        const auto ret = FunctionCast(wrapGcHwInit, orgGcHwInit)(gc, input, output);
        BCLOG("BC250HWL", "GC hw_init (%s) <<< 0x%X after %u write(s) (+%u SPM sample-delay writes dropped), %u read(s)",
            gcLive ? "live" : "dry run", ret, gcWrites, gcSpmWrites, gcReads);
        return ret;
    }

    // Level 18: GC's hardware config from IP discovery. TTL's GC wrapper asks the BGM for the discovery GC table
    // (IpiBgmGetIpConfigFromDiscovery 0xc3a97d7: ipi, block, {buffer, size, found}) into the buffer GC's sw_init sized
    // for the table version it expects (Navi 10's). The BC-250's table is gc_info v1.1, 0x64 bytes, and the BGM
    // refuses a smaller buffer ("HW config info buffer too small!"), so GC's hw_init never ran. Later gc_info versions
    // only append fields, and GC's reader (0xc30fc68) checks id 'GC', size == its buffer size and the major/minor it
    // expects (it parses 1.0 and 1.1). The table is read into a scratch buffer and copied truncated to the caller's
    // buffer, its header size set to that size and its minor to the version that size is (1.0: 0x50, 1.1: 0x5C+).
    const UInt8 kIpiBgmGetIpConfigPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC,
        0x28, 0x49, 0x89, 0xFE, 0x31, 0xC0, 0x48, 0x89, 0x45, 0xD8, 0x48, 0x89, 0x45, 0xD0, 0x48, 0x89, 0x45, 0xC8,
        0xC7};
    constexpr UInt32 kGcInfoBlock = 0xB, kGcInfoV10Size = 0x50, kGcInfoV11Size = 0x5C;

    struct IpConfigBuffer
    {
        UInt8* data;
        UInt32 size;
        UInt32 pad;
        UInt8  found;
    };

    mach_vm_address_t orgIpiBgmGetIpConfig = 0;

    UInt8 wrapIpiBgmGetIpConfig(void* ipi, UInt32 block, IpConfigBuffer* buffer)
    {
        auto ret = FunctionCast(wrapIpiBgmGetIpConfig, orgIpiBgmGetIpConfig)(ipi, block, buffer);
        if (block != kGcInfoBlock || buffer == nullptr || buffer->data == nullptr || (ret != 0 && buffer->found)) {
            return ret;
        }
        UInt8          scratch[0x200] {};
        IpConfigBuffer big {scratch, sizeof(scratch), 0, 0};
        const auto     bigRet = FunctionCast(wrapIpiBgmGetIpConfig, orgIpiBgmGetIpConfig)(ipi, block, &big);
        const auto     tableSize = getMember<UInt32>(scratch, 8);
        if (bigRet == 0 || !big.found || tableSize < kGcInfoV10Size || buffer->size < kGcInfoV10Size) {
            BCLOG("BC250HWL", "GC info: discovery table not read (ret %u, found %u, size 0x%X, buffer 0x%X)", bigRet,
                big.found, tableSize, buffer->size);
            return ret;
        }
        const auto copy = buffer->size < tableSize ? buffer->size : tableSize;
        memcpy(buffer->data, scratch, copy);
        getMember<UInt32>(buffer->data, 8) = copy;
        getMember<UInt16>(buffer->data, 6) = copy >= kGcInfoV11Size ? 1 : 0;
        buffer->found = 1;
        BCLOG("BC250HWL", "GC info: discovery table v%u.%u, 0x%X bytes, given as v1.%u, 0x%X bytes (GC's buffer)",
            getMember<UInt16>(scratch, 4), getMember<UInt16>(scratch, 6), tableSize,
            getMember<UInt16>(buffer->data, 6), copy);
        return 1;
    }

    // Level 18: RLC safe mode. After the golden settings GC 10.1 enters RLC safe mode (gc_enter_rlc_safe_mode_10_1
    // 0xc31a69e, exit 0xc31a7dd) and fails with 3 ("RLC is not enabled!") when RLC_CNTL.RLC_ENABLE is clear, as it is on
    // the BC-250 at that point (nothing has started RLC yet; Apple's Navi 10 path expects its own SOS to have). Linux's
    // gfx_v10_0_set_safe_mode/unset_safe_mode return without doing anything while RLC is not enabled: so do these.
    const UInt8 kGcEnterSafeModePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83,
        0xEC, 0x20, 0x48, 0x89, 0xFB, 0x4C, 0x8D, 0xBF, 0xC0, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xFF, 0xBE, 0x00, 0x4C,
        0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xE8, 0x2A};
    const UInt8 kGcExitSafeModePattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xFB,
         0x4C, 0x8D, 0xBF, 0xC0, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xFF, 0xBE, 0x00, 0x4C, 0x00, 0x00, 0xBA, 0x01, 0x00,
         0x00, 0x00, 0xE8, 0xF0};
    constexpr UInt32 kGcRlcNotEnabled = 3;

    mach_vm_address_t orgGcEnterSafeMode = 0, orgGcExitSafeMode = 0;
    UInt32            gcSafeModeSkips    = 0;

    UInt32 gcSafeModeResult(const char* what, UInt32 ret)
    {
        if (ret != kGcRlcNotEnabled) { return ret; }
        if (++gcSafeModeSkips <= 8) {
            BCLOG("BC250HWL", "GC: RLC not enabled, safe mode %s skipped as Linux does", what);
        }
        return 0;
    }

    UInt32 wrapGcEnterSafeMode(void* gc)
    {
        return gcSafeModeResult("entry", FunctionCast(wrapGcEnterSafeMode, orgGcEnterSafeMode)(gc));
    }

    UInt32 wrapGcExitSafeMode(void* gc)
    {
        return gcSafeModeResult("exit", FunctionCast(wrapGcExitSafeMode, orgGcExitSafeMode)(gc));
    }

    // Level 18: GC's error messages. GC reports every failure through 0xc30ff25(gc, level, function, file, line,
    // message, ...), which is empty in Apple's build (push rbp; mov rbp, rsp; pop rbp; ret): the messages are lost.
    // It is routed with a 5-byte jump (it has 6 bytes) to a logger; the original is never called.
    const UInt8 kGcLogPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3, 0xB8, 0x02, 0x00, 0x00, 0x00, 0x48, 0x85, 0xFF,
        0x74, 0x3C};
    constexpr UInt32 kGcLogMax = 60;
    UInt32           gcLogs    = 0;

    void wrapGcLog(void* gc, UInt32 level, const char* function, const char* file, UInt32 line, const char* message)
    {
        (void)gc;
        (void)file;
        if (++gcLogs <= kGcLogMax) {
            BCLOG("BC250HWL", "GC: %s:%u %s (level %u)", hwlibsString(function), line, hwlibsString(message), level);
        }
    }

    void hookGcLog(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        // On 26 _gc_debug_print is one empty body shared by 46 debug/trace symbols (PSP, PowerPlay, ...): the
        // wrapper only dereferences strings inside HWLibs.
        hwlibsStart   = slide;
        hwlibsEnd     = slide + size;
        const auto at = findFunction(patcher, id, "_gc_debug_print", kGcLogPattern, sizeof(kGcLogPattern), slide, size);
        KernelPatcher::RouteRequest request {nullptr, wrapGcLog};
        request.from = at;
        if (at == 0 || !patcher.routeMultipleShort(id, &request, 1)) {
            BCLOG("BC250HWL", "level 18: GC error log %s; GC messages not captured", at ? "failed to route" : "not found");
            patcher.clearError();
        }
    }

    // Level 18: RLC start. Apple's GC 10.1 path expects RLC autoload (its own SOS on Navi 10 starts the RLC, which
    // fetches the CP microcode): gc_check_rlc_autoload_ucode_loaded_10_1 (0xc3153f2) waits for it and fails on the
    // BC-250 ("RLC fw autoload timeout!", "CP is busy!"). Linux runs PSP 11.0.8 without autoload: PSP loads each image
    // and gfx_v10_0_rlc_resume then stops the RLC, turns off CG (RLC_CGCG_CGLS_CTRL = 0) and PG (RLC_PG_CNTL = 0) and
    // starts it (rlc_start: RLC_PG_CNTL bit 23, no RLC-SMU handshake without GFXOFF; RLC_CNTL.RLC_ENABLE_F32; 50 us).
    // Apple's code after the wait (clear-state buffer, SRM) is Linux's autoload-branch order, with the RLC running.
    // The wait is replaced by that sequence through GC's own register functions (logged, not made, in the dry run).
    const UInt8 kGcRlcAutoloadCheckPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x45, 0x31,
        0xF6, 0x80, 0xBF, 0x22};
    constexpr UInt32 kGcBase1 = 0xA000, kGcBlock = 0xB;
    constexpr UInt32 kRlcCntl = kGcBase1 + 0x4C00, kRlcCgcgCglsCtrl = kGcBase1 + 0x4C49, kRlcPgCntl = kGcBase1 + 0x4C43;
    constexpr UInt32 kRlcEnableF32 = 1U << 0, kRlcPgNoSmuHandshake = 1U << 23;

    mach_vm_address_t orgGcRlcAutoloadCheck = 0;

    UInt32 gcReadReg(void* gc, UInt32 reg) { return wrapGcRead(gc, reg, kGcBlock, 0); }
    void   gcWriteReg(void* gc, UInt32 reg, UInt32 value) { wrapGcWrite(gc, reg, value, kGcBlock, 0); }

    void gcRlcStart(void* gc)
    {
        constexpr UInt32 kRlcStat = 0xA000 + 0x4C04, kRlcGpmStat = 0xA000 + 0x4E6E, kRlcBootload = 0xA000 + 0x4E8D;
        constexpr UInt32 kRlcGpmGeneral6 = 0xA000 + 0x4C69;
        gcWriteReg(gc, kRlcCntl, gcReadReg(gc, kRlcCntl) | kRlcEnableF32);
        IODelay(50);
        BCLOG("BC250HWL", "GC: RLC started%s: RLC_CNTL 0x%08X, RLC_STAT 0x%08X, RLC_GPM_STAT 0x%08X, bootload 0x%08X, "
                          "GPM_GENERAL_6 0x%08X, RLC_PG_CNTL 0x%08X",
            gcLive ? "" : " (dry run)", gcReadReg(gc, kRlcCntl), gcReadReg(gc, kRlcStat), gcReadReg(gc, kRlcGpmStat),
            gcReadReg(gc, kRlcBootload), gcReadReg(gc, kRlcGpmGeneral6), gcReadReg(gc, kRlcPgCntl));
        IODelay(1000);
        BCLOG("BC250HWL", "GC: 1 ms later: RLC_CNTL 0x%08X, RLC_STAT 0x%08X, RLC_GPM_STAT 0x%08X", gcReadReg(gc, kRlcCntl),
            gcReadReg(gc, kRlcStat), gcReadReg(gc, kRlcGpmStat));
        // Diagnostic: RLC_CNTL reads 0 after the start. Which of its bits hold (READ_CACHE_DISABLE, bit 2, is inert)
        // tells a write-protected register from an F32 that will not stay enabled; GRBM status shows what is busy.
        if (!gcLive || (gcReadReg(gc, kRlcCntl) & kRlcEnableF32) != 0) { return; }
        constexpr UInt32 kRlcReadCacheDisable = 1U << 2, kGrbmStatus = 0x1260 + 0xDA4, kGrbmStatus2 = 0x1260 + 0xDA2;
        constexpr UInt32 kRlcSafeMode = 0xA000 + 0x4C05;
        gcWriteReg(gc, kRlcCntl, kRlcReadCacheDisable);
        const UInt32 bit2 = gcReadReg(gc, kRlcCntl);
        gcWriteReg(gc, kRlcCntl, kRlcReadCacheDisable | kRlcEnableF32);
        const UInt32 both = gcReadReg(gc, kRlcCntl);
        gcWriteReg(gc, kRlcCntl, kRlcEnableF32);
        IODelay(50);
        BCLOG("BC250HWL", "GC: RLC_CNTL probe: 0x4 -> 0x%08X, 0x5 -> 0x%08X, 0x1 -> 0x%08X; RLC_SAFE_MODE 0x%08X, "
                          "GRBM_STATUS 0x%08X, GRBM_STATUS2 0x%08X",
            bit2, both, gcReadReg(gc, kRlcCntl), gcReadReg(gc, kRlcSafeMode), gcReadReg(gc, kGrbmStatus),
            gcReadReg(gc, kGrbmStatus2));
        // How long the F32 stays enabled, and where it stops: its microcode version, thread enables, exception and
        // debug-instruction registers (all read-only here).
        UInt32 t[6] {};
        gcWriteReg(gc, kRlcCntl, kRlcEnableF32);
        t[0] = gcReadReg(gc, kRlcCntl);
        for (UInt32 i = 1; i < arrsize(t); i++) {
            IODelay(i == 1 ? 1 : (i == 2 ? 4 : 10));
            t[i] = gcReadReg(gc, kRlcCntl);
        }
        BCLOG("BC250HWL", "GC: RLC_CNTL after enable: 0us 0x%X, 1us 0x%X, 5us 0x%X, 15us 0x%X, 25us 0x%X, 35us 0x%X", t[0],
            t[1], t[2], t[3], t[4], t[5]);
        auto r = [gc](UInt32 reg) { return gcReadReg(gc, kGcBase1 + reg); };
        BCLOG("BC250HWL", "GC: RLC F32_UCODE_VERSION 0x%08X, UCODE_CNTL 0x%08X, GPM_THREAD_ENABLE 0x%08X, "
                          "GPM_THREAD_RESET 0x%08X, SMU_SAFE_MODE 0x%08X, SPM_MC_CNTL 0x%08X",
            r(0x4C03), r(0x4C27), r(0x4C45), r(0x4C28), r(0x4C09), r(0x4C71));
        BCLOG("BC250HWL", "GC: RLC EXCEPTION_REG_1-4 0x%08X 0x%08X 0x%08X 0x%08X, DEBUG_INST_ADDR 0x%08X, A 0x%08X, "
                          "B 0x%08X, GPM_STAT_2 0x%08X, BOOTLOAD_ID_STATUS 0x%08X 0x%08X",
            r(0x4E62), r(0x4E63), r(0x4E64), r(0x4E65), r(0x4C1D), r(0x4C22), r(0x4C23), r(0x4E75), r(0x4EEC), r(0x4EED));
    }

    // Reads physical memory (the BC-250's VRAM is a carve-out of system RAM, so the CPU reaches it directly).
    bool bc250ReadPhys(UInt64 phys, void* buffer, UInt32 length)
    {
        auto* desc = IOMemoryDescriptor::withAddressRange(phys, length, kIODirectionIn, TASK_NULL);
        if (desc == nullptr) { return false; }
        bool ok  = false;
        auto* map = desc->map(kIOMapInhibitCache);
        if (map != nullptr) {
            memcpy(buffer, reinterpret_cast<const void*>(map->getVirtualAddress()), length);
            map->release();
            ok = true;
        }
        desc->release();
        return ok;
    }

    // The GART entry (context 0, flat table) for a GPU address, and the first dwords behind it if it is valid.
    void gcGartDump(const char* what, UInt64 pdb, UInt64 start, UInt64 end, UInt64 va)
    {
        if (va < start || va > end) {
            BCLOG("BC250HWL", "GART %s 0x%llX: outside context 0 (0x%llX-0x%llX)", what, va, start, end);
            return;
        }
        const UInt64 entryAddr = pdb + ((va - start) >> 12) * 8;
        UInt64       pte       = 0;
        if (!bc250ReadPhys(entryAddr, &pte, sizeof(pte))) {
            BCLOG("BC250HWL", "GART %s 0x%llX: entry at 0x%llX not readable", what, va, entryAddr);
            return;
        }
        const UInt64 page = pte & 0x0000FFFFFFFFF000ULL;
        BCLOG("BC250HWL", "GART %s 0x%llX: entry at 0x%llX = 0x%016llX (valid %u, system %u, snooped %u, page 0x%llX)", what,
            va, entryAddr, pte, static_cast<UInt32>(pte & 1), static_cast<UInt32>((pte >> 1) & 1),
            static_cast<UInt32>((pte >> 2) & 1), page);
        if ((pte & 1) == 0) { return; }
        UInt32 data[32] {};
        const UInt32 length = static_cast<UInt32>(MIN(sizeof(data), 0x1000 - (va & 0xFFF)));
        if (bc250ReadPhys(page + (va & 0xFFF), data, length)) {
            for (UInt32 i = 0; i < length / 4; i += 8) {
                BCLOG("BC250HWL", "GART %s data +0x%02X: %08X %08X %08X %08X %08X %08X %08X %08X", what, i * 4, data[i],
                    data[i + 1], data[i + 2], data[i + 3], data[i + 4], data[i + 5], data[i + 6], data[i + 7]);
            }
        }
    }

    // A GPU address as a system physical one: the FB (carve-out at the FB offset) or GART context 0 (flat table).
    bool gpuToPhys(UInt64 va, UInt64* phys)
    {
        if (va >= gvmFbBase && va < gvmFbBase + gvmFbSize) {
            *phys = va - gvmFbBase + gvmFbOffset;
            return true;
        }
        auto&        nred  = NRed::singleton();
        auto         r     = [&nred](UInt32 off) { return static_cast<UInt64>(nred.readReg32(0x1260 + off)); };
        const UInt64 pdb   = ((r(0x168C) << 32) | r(0x168B)) & ~0xFFFULL;
        const UInt64 start = ((r(0x16AC) << 32) | r(0x16AB)) << 12;
        const UInt64 end   = ((r(0x16CC) << 32) | r(0x16CB)) << 12;
        UInt64       pte   = 0;
        if (va < start || va > end || !bc250ReadPhys(pdb + ((va - start) >> 12) * 8, &pte, sizeof(pte)) ||
            (pte & 1) == 0) {
            return false;
        }
        *phys = (pte & 0x0000FFFFFFFFF000ULL) | (va & 0xFFF);
        return true;
    }

    // Level 27: the SOS's answers to the KM ring's commands. Firmware loads are only queued (per-image buffers with a
    // handle), so their status was never read. Each 64-byte frame (psp_gfx_rb_frame) points to its command buffer
    // (psp_gfx_cmd_resp: +8 cmd_id, +0x28 fw_type for LOAD_IP_FW, +0x360 resp.status) and a fence (address, value).
    constexpr UInt64 kPspKmRing = 0xF40FBFF000;    // Apple's KM ring (C2PMSG_69/70 at ring create), 0x1000 bytes.
    void pspRingStatusDump()
    {
        UInt64 ringPhys = 0;
        if (!gpuToPhys(kPspKmRing, &ringPhys)) {
            BCLOG("BC250HWL", "PSP ring: 0x%llX not translatable", kPspKmRing);
            return;
        }
        const auto wptr = NRed::singleton().readReg32(0x16000 + 0x83);    // MP0 C2PMSG_67, in dwords.
        BCLOG("BC250HWL", "PSP ring 0x%llX (phys 0x%llX), C2PMSG_67 wptr 0x%X", kPspKmRing, ringPhys, wptr);
        for (UInt32 i = 0; i < 16 && i * 16 < wptr; i++) {
            UInt32 frame[16] {};
            if (!bc250ReadPhys(ringPhys + i * 64, frame, sizeof(frame))) { break; }
            const UInt64 cmdVa   = (static_cast<UInt64>(frame[1]) << 32) | frame[0];
            const UInt64 fenceVa = (static_cast<UInt64>(frame[4]) << 32) | frame[3];
            UInt64       cmdPhys = 0, fencePhys = 0;
            UInt32       head[12] {}, resp[4] {}, fence = 0xDEADBEEF;
            const bool   cmdOk = gpuToPhys(cmdVa, &cmdPhys) && bc250ReadPhys(cmdPhys, head, sizeof(head)) &&
                               bc250ReadPhys(cmdPhys + 0x360, resp, sizeof(resp));
            if (gpuToPhys(fenceVa, &fencePhys)) { bc250ReadPhys(fencePhys, &fence, sizeof(fence)); }
            BCLOG("BC250HWL", "PSP frame %u: cmd 0x%llX%s id %u fw_type %u, status 0x%08X (resp %08X %08X %08X), fence "
                              "0x%llX = 0x%X (frame's 0x%X)",
                i, cmdVa, cmdOk ? "" : " (unreadable)", head[2], head[10], resp[0], resp[1], resp[2], resp[3], fenceVa,
                fence, frame[5]);
        }
    }

    // Busy/stall state of the CP, the GFX ring (ME) and the VM L2, compact, for comparisons over time.
    void gcCpBrief(const char* when)
    {
        auto r = [](UInt32 off) { return NRed::singleton().readReg32(0x1260 + off); };
        BCLOG("BC250HWL", "GC CP (%s): STAT 0x%08X CPC 0x%08X/BUSY 0x%08X/STALL 0x%08X CPF 0x%08X/BUSY 0x%08X/STALL "
                          "0x%08X GRBM 0x%08X/2 0x%08X L2 0x%X",
            when, r(0xF40), r(0xE24), r(0xE25), r(0xE26), r(0xE27), r(0xE28), r(0xE29), r(0xDA4), r(0xDA2), r(0x15E3));
        BCLOG("BC250HWL", "GC GFX (%s): ME_CNTL 0x%08X, IP PFP/ME/CE/MEC1/MEC2 0x%X/0x%X/0x%X/0x%X/0x%X, RB0 BASE 0x%X:%08X "
                          "CNTL 0x%08X RPTR 0x%X WPTR 0x%X:%X RPTR_ADDR 0x%08X, RB_DOORBELL 0x%08X, WPTR_POLL_CNTL 0x%08X",
            when, r(0xF56), r(0xF45), r(0xF46), r(0xF47), r(0xF48), r(0xF49), r(0x1E51), r(0x1DE0), r(0x1DE1), r(0xF60),
            r(0x1DF5), r(0x1DF4), r(0x1DE3), r(0x1E8D), r(0xF62));
        // Level 28: the VM side of a GPU that stops with the L2 busy: faults (status, address, both hubs), the 18
        // invalidation engines' ACKs, and both SDMA engines (status, GFX ring read/write pointers).
        auto a = [](UInt32 abs) { return NRed::singleton().readReg32(abs); };
        UInt32 ack[18];
        for (UInt32 i = 0; i < 18; i++) { ack[i] = r(0x1655 + i); }
        BCLOG("BC250HWL", "GC VM (%s): fault 0x%08X addr 0x%X:%08X, MM fault 0x%08X addr 0x%X:%08X, ACK0-17 %X %X %X %X %X %X "
                          "%X %X %X %X %X %X %X %X %X %X %X %X",
            when, r(0x15EC), r(0x15EE), r(0x15ED), a(0x1A68C), a(0x1A68E), a(0x1A68D), ack[0], ack[1], ack[2], ack[3],
            ack[4], ack[5], ack[6], ack[7], ack[8], ack[9], ack[10], ack[11], ack[12], ack[13], ack[14], ack[15], ack[16],
            ack[17]);
        BCLOG("BC250HWL", "SDMA (%s): 0 STATUS 0x%08X RPTR 0x%X WPTR 0x%X CNTL 0x%08X, 1 STATUS 0x%08X RPTR 0x%X WPTR 0x%X "
                          "CNTL 0x%08X; CP_INT_CNTL_RING0 0x%08X",
            when, a(0x1285), a(0x12E3), a(0x12E5), a(0x127C), a(0x1885), a(0x18E3), a(0x18E5), a(0x187C), r(0x1E0A));
    }

    void gcCpSnapshot(void* gc)
    {
        auto r = [gc](UInt32 off) { return gcReadReg(gc, 0x1260 + off); };
        gcCpBrief("timeout");
        pspRingStatusDump();
        const UInt32 ip1 = r(0xF48);
        IODelay(100);
        BCLOG("BC250HWL", "GC CP: STAT 0x%08X, CPC_STATUS 0x%08X, CPC_BUSY 0x%08X, CPC_STALLED1 0x%08X, CPF_STATUS 0x%08X, "
                          "GRBM_STATUS 0x%08X, CP_INT_STATUS 0x%08X",
            r(0xF40), r(0xE24), r(0xE25), r(0xE26), r(0xE27), r(0xDA4), r(0x1DEA));
        BCLOG("BC250HWL", "GC CP: MEC1_INSTR_PNTR 0x%X then 0x%X, MEC2 0x%X, MEC doorbell range 0x%X-0x%X, RB 0x%X-0x%X", ip1,
            r(0xF48), r(0xF49), r(0x1DFC), r(0x1DFD), r(0x1DFA), r(0x1DFB));
        gcWriteReg(gc, 0x1260 + 0xDC2, 0x9);    // GRBM_GFX_CNTL: the KIQ's ME/pipe/queue, as Apple's KIQ init.
        BCLOG("BC250HWL", "GC KIQ: HQD_ACTIVE 0x%X, PQ_RPTR 0x%X, PQ_WPTR 0x%X:%X, DOORBELL_CONTROL 0x%08X, DEQUEUE_REQUEST 0x%X, "
                          "PQ_CONTROL 0x%08X, ME1_PIPE0_INT_STATUS 0x%08X",
            r(0x1FAB), r(0x1FB3), r(0x1FE0), r(0x1FDF), r(0x1FB8), r(0x1FC1), r(0x1FBA), r(0x1E2D));
        BCLOG("BC250HWL", "GC KIQ: HQD_ERROR 0x%08X, HQ_STATUS0 0x%08X, HQ_STATUS1 0x%08X, DEQUEUE_STATUS 0x%08X, "
                          "IB_CONTROL 0x%08X, EOP_EVENTS 0x%08X",
            r(0x1FDC), r(0x1FC9), r(0x1FCC), r(0x1FE8), r(0x1FBE), r(0x1FD3));
        const UInt64 pqBase  = ((static_cast<UInt64>(r(0x1FB2)) << 32) | r(0x1FB1)) << 8;
        const UInt64 rptrRep = (static_cast<UInt64>(r(0x1FB5)) << 32) | r(0x1FB4);
        const UInt64 wptrPol = (static_cast<UInt64>(r(0x1FB7)) << 32) | r(0x1FB6);
        const UInt64 mqd     = (static_cast<UInt64>(r(0x1FAA)) << 32) | r(0x1FA9);
        gcWriteReg(gc, 0x1260 + 0xDC2, 0);
        const UInt64 pdb   = ((static_cast<UInt64>(r(0x168C)) << 32) | r(0x168B)) & ~0xFFFULL;
        const UInt64 start = ((static_cast<UInt64>(r(0x16AC)) << 32) | r(0x16AB)) << 12;
        const UInt64 end   = ((static_cast<UInt64>(r(0x16CC)) << 32) | r(0x16CB)) << 12;
        BCLOG("BC250HWL", "GC KIQ: PQ_BASE 0x%llX, RPTR_REPORT 0x%llX, WPTR_POLL 0x%llX, MQD 0x%llX", pqBase, rptrRep,
            wptrPol, mqd);
        UInt32 h1[6], h2[6];
        for (UInt32 i = 0; i < 6; i++) {
            h1[i] = r(0xE2E);
            h2[i] = r(0xE2F);
        }
        BCLOG("BC250HWL", "GC CP: ME1 headers %08X %08X %08X %08X %08X %08X, ME2 headers %08X %08X %08X %08X %08X %08X",
            h1[0], h1[1], h1[2], h1[3], h1[4], h1[5], h2[0], h2[1], h2[2], h2[3], h2[4], h2[5]);
        BCLOG("BC250HWL", "GC CP: HPD_UTCL1_CNTL 0x%08X ERROR 0x%08X ADDR 0x%08X, GCVM_L2_STATUS 0x%08X, ME2 pipe0/1 INT 0x%08X "
                          "0x%08X",
            r(0x1FA6), r(0x1FA7), r(0x1FA8), r(0x15E3), r(0x1E31), r(0x1E32));
        gcGartDump("KIQ ring", pdb, start, end, pqBase);
        gcGartDump("KIQ rptr report", pdb, start, end, rptrRep);
        // VM: a fetch stuck on translation shows here (fault status/address), and GART context 0 as GVM set it up.
        BCLOG("BC250HWL", "GC VM: FAULT_STATUS 0x%08X ADDR 0x%X:%08X, MM FAULT_STATUS 0x%08X ADDR 0x%X:%08X",
            r(0x15EC), r(0x15EE), r(0x15ED), gcReadReg(gc, 0x1A000 + 0x68C), gcReadReg(gc, 0x1A000 + 0x68E),
            gcReadReg(gc, 0x1A000 + 0x68D));
        BCLOG("BC250HWL", "GC VM: CONTEXT0_CNTL 0x%08X PDB 0x%X:%08X START 0x%X:%08X END 0x%X:%08X",
            r(0x1620), r(0x168C), r(0x168B), r(0x16AC), r(0x16AB), r(0x16CC), r(0x16CB));
        BCLOG("BC250HWL", "GC VM: FB 0x%X-0x%X OFFSET 0x%X, SYS APERTURE 0x%X-0x%X, AGP 0x%X-0x%X, FAULT_CNTL 0x%08X",
            r(0x1720), r(0x1721), r(0x170B), r(0x1725), r(0x1726), r(0x1723), r(0x1722), r(0x15E8));
    }

    // bc250cu=40: the 40-CU unlock the BC-250 community uses on Linux (duggasco/bc250-40cu-unlock; facts only, no code
    // taken). IP discovery already describes the full GC (2 SEs x 2 SAs x 5 WGPs = 40 CUs); the fuse harvest
    // (CC_GC_SHADER_ARRAY_CONFIG.INACTIVE_WGPS 0xFFF8: WGPs 3-4 off, 24 CUs) and the SPI's static WGP power-gating mask
    // (0x7) disable the rest. Before GC's hw_init reads them (it counts CUs per SE/SA, as Linux's constants_init
    // does before the RLC starts), each SA gets CC_GC_SHADER_ARRAY_CONFIG = 0, SPI_PG_ENABLE_STATIC_WGP_MASK = 0x1F and
    // RLC_PG_ALWAYS_ON_WGP_MASK = 0x1F. Only boards whose every SA reads the stock harvest 0xFFF80000 are changed
    // (anything else may be a real defect). The GPU draws more power (+30 W at 1500 MHz on Linux); a lower GPU clock
    // (bc250gfxmhz) is the community's sweet spot (1500 MHz / 900 mV).
    void gcUnlockCus(void* gc)
    {
        static UInt32 target = 0;
        static bool   parsed = false;
        if (!parsed) {
            parsed = true;
            PE_parse_boot_argn("bc250cu", &target, sizeof(target));
            if (target != 0 && target != 40) {
                BCLOG("BC250HWL", "bc250cu=%u ignored (only 40)", target);
                target = 0;
            }
        }
        if (target != 40) { return; }
        constexpr UInt32 kGrbmGfxIndex = 0xA000 + 0x2200, kCcShaderArrayConfig = 0x1260 + 0x100F,
                         kSpiStaticWgpMask = 0x1260 + 0x1277, kRlcAlwaysOnWgpMask = 0xA000 + 0x4C53,
                         kBroadcastAll = 0xE0000000, kInstanceBroadcast = 0x40000000, kStockHarvest = 0xFFF80000,
                         kInactiveWgpsMask = 0xFFFF0000, kAllWgps = 0x1F;
        constexpr UInt32 kSes = 2, kSas = 2;
        UInt32           before[kSes][kSas] {};
        for (UInt32 se = 0; se < kSes; se++) {
            for (UInt32 sa = 0; sa < kSas; sa++) {
                gcWriteReg(gc, kGrbmGfxIndex, kInstanceBroadcast | (se << 16) | (sa << 8));
                before[se][sa] = gcReadReg(gc, kCcShaderArrayConfig);
            }
        }
        bool stock = true;
        for (auto& row : before) {
            for (auto value : row) { stock &= value == kStockHarvest; }
        }
        if (!stock) {
            gcWriteReg(gc, kGrbmGfxIndex, kBroadcastAll);
            const bool unlocked = ((before[0][0] | before[0][1] | before[1][0] | before[1][1]) & 0x001F0000) == 0;
            BCLOG("BC250HWL", "bc250cu=40: harvest %08X %08X %08X %08X is %s; not changed", before[0][0], before[0][1],
                before[1][0], before[1][1], unlocked ? "already 40 CUs" : "not the stock 0xFFF80000");
            return;
        }
        for (UInt32 se = 0; se < kSes; se++) {
            for (UInt32 sa = 0; sa < kSas; sa++) {
                gcWriteReg(gc, kGrbmGfxIndex, kInstanceBroadcast | (se << 16) | (sa << 8));
                const UInt32 spi = gcReadReg(gc, kSpiStaticWgpMask);
                gcWriteReg(gc, kCcShaderArrayConfig, 0);
                gcWriteReg(gc, kSpiStaticWgpMask, kAllWgps);
                gcWriteReg(gc, kRlcAlwaysOnWgpMask, kAllWgps);
                const UInt32 cc = gcReadReg(gc, kCcShaderArrayConfig);
                BCLOG("BC250HWL", "bc250cu=40: SE%u SA%u CC_GC_SHADER_ARRAY_CONFIG 0x%08X -> 0x%08X (inactive WGPs 0x%X), "
                                  "SPI_PG_ENABLE_STATIC_WGP_MASK 0x%X -> 0x%X, RLC_PG_ALWAYS_ON_WGP_MASK 0x%X",
                    se, sa, before[se][sa], cc, (cc & kInactiveWgpsMask) >> 16 & kAllWgps, spi,
                    gcReadReg(gc, kSpiStaticWgpMask), gcReadReg(gc, kRlcAlwaysOnWgpMask));
            }
        }
        gcWriteReg(gc, kGrbmGfxIndex, kBroadcastAll);
    }

    UInt32 wrapGcRlcAutoloadCheck(void* gc)
    {
        BCLOG("BC250HWL", "GC: RLC autoload check replaced by Linux's RLC resume (stop, CG/PG off, start after the CSB)");
        gcWriteReg(gc, kRlcCntl, gcReadReg(gc, kRlcCntl) & ~kRlcEnableF32);
        gcWriteReg(gc, kRlcCgcgCglsCtrl, 0);
        gcWriteReg(gc, kRlcPgCntl, 0);
        gcWriteReg(gc, kRlcPgCntl, gcReadReg(gc, kRlcPgCntl) | kRlcPgNoSmuHandshake);
        gcRlcStartPending = true;    // Started once the clear-state buffer is set (gcWriteCommon), Linux's order.
        return 0;
    }

    // Level 18: GFX clock gating. gc_control_power_features_10_1 (0xc319b5c: gc, enable) turns on each feature of
    // the mask at gc+0xD98 (bits 0-1 MGCG with memory light sleep, 2-3 CGCG/CGLS, 8-9 and others), which GC builds from
    // its defaults (Navi 10's). On the board it enabled RLC_CGCG_CGLS_CTRL 0x363F, the MGCG override, CP/RLC memory
    // light sleep and the per-CU CGTS registers. Linux runs Cyan Skillfish with cg_flags = pg_flags = 0: the mask is
    // cleared before the call (the rest of the function, CP_INT_CNTL_RING0's interrupt enables, still runs).
    const UInt8 kGcPowerFeaturesPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
        0x50, 0x48, 0x89, 0xFB, 0x48, 0x8B, 0x87, 0x88};
    constexpr UInt32  kGcFeatureMask = 0xD98;
    mach_vm_address_t orgGcPowerFeatures = 0;

    UInt32 wrapGcPowerFeatures(void* gc, UInt32 enable)
    {
        auto& mask = getMember<UInt32>(gc, kGcFeatureMask);
        BCLOG("BC250HWL", "GC: power features (enable %u): Apple's mask 0x%X cleared (Linux: cg_flags = 0)", enable & 0xFF,
            mask);
        mask = 0;
        return FunctionCast(wrapGcPowerFeatures, orgGcPowerFeatures)(gc, enable);
    }

    // Level 18: GFX power settings Linux leaves to the SMU firmware or does not program on Cyan Skillfish:
    //  - 0xc31ccc6 writes Navi 10's fixed CAC weight (GC_CAC_IND 1 = 0xF9C801), GC_EDC_STATUS (0xA024) and DIDT
    //    (DIDT_IND 0x1A = 4); gfx_v10_0 never touches them (the SMU owns CAC/EDC/DIDT);
    //  - 0xc319a7f writes the 32 per-CU CGTS_*_TCP_CTRL/SM_CTRL clock-gating timings, inert with clock gating off and
    //    not written by Linux.
    // Both are answered with success without running.
    const UInt8 kGcCacDidtPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D, 0xB7, 0xC0,
        0x00, 0x00, 0x00, 0x4C, 0x89, 0xF7, 0xBE, 0x00, 0x22};
    const UInt8 kGcCgtsPattern[]    = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50,
           0x48, 0x89, 0xFB, 0x4C, 0x8D, 0xB7, 0xC0, 0x00, 0x00, 0x00, 0x45, 0x31, 0xE4, 0x4C, 0x8D, 0x2D, 0x3F};
    mach_vm_address_t orgGcCacDidt = 0, orgGcCgts = 0;

    UInt32 wrapGcCacDidt(void* gc)
    {
        (void)gc;
        BCLOG("BC250HWL", "GC: CAC/EDC/DIDT setup skipped (Linux leaves it to the SMU)");
        return 0;
    }

    UInt32 wrapGcCgts(void* gc)
    {
        (void)gc;
        BCLOG("BC250HWL", "GC: per-CU CGTS clock-gating timings skipped (clock gating off, as Linux)");
        return 0;
    }

    // Returns false if a write path could not be hooked; GC's hw_init must then not run.
    bool hookGcDryRun(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        struct Hook
        {
            const char*        name;
            const char*        symbol;
            const UInt8*       pattern;
            size_t             length;
            mach_vm_address_t  wrapper;
            mach_vm_address_t& org;
            bool               guard;
        };
        Hook hooks[] = {
            {"write", "_gc_cgs_write_register", kGcWritePattern, sizeof(kGcWritePattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcWrite),
                orgGcWrite, true},
            {"write_ext", "_gc_cgs_write_register_ext", kGcWriteExtPattern, sizeof(kGcWriteExtPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcWriteExt), orgGcWriteExt, true},
            {"write_ext2", "_gc_cgs_write_register_ext2", kGcWriteExt2Pattern, sizeof(kGcWriteExt2Pattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcWriteExt2), orgGcWriteExt2, true},
            {"read", "_gc_cgs_read_register", kGcReadPattern, sizeof(kGcReadPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcRead), orgGcRead,
                false},
            {"read_ext2", "_gc_cgs_read_register_ext2", kGcReadExt2Pattern, sizeof(kGcReadExt2Pattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcReadExt2), orgGcReadExt2, false},
            {"golden query", "_gc_cgs_query_golden_settings", kGcGoldenPattern, sizeof(kGcGoldenPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcGolden),
                orgGcGolden, false},
            {"hw_init", "_gc_hw_init", kGcHwInitPattern, sizeof(kGcHwInitPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcHwInit),
                orgGcHwInit, false},
            {"RLC autoload check", "_gc_check_ucode_loaded_10_1", kGcRlcAutoloadCheckPattern,
                sizeof(kGcRlcAutoloadCheckPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcRlcAutoloadCheck), orgGcRlcAutoloadCheck, false},
            {"RLC safe mode entry", "_gc_enter_rlc_safe_mode_10_1", kGcEnterSafeModePattern,
                sizeof(kGcEnterSafeModePattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcEnterSafeMode), orgGcEnterSafeMode, false},
            {"RLC safe mode exit", "_gc_exit_rlc_safe_mode_10_1", kGcExitSafeModePattern,
                sizeof(kGcExitSafeModePattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcExitSafeMode), orgGcExitSafeMode, false},
            {"firmware check", "_gc_cgs_query_fw_load_status", kGcFwLoadedPattern, sizeof(kGcFwLoadedPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcFwLoaded), orgGcFwLoaded, false},
            {"CAC/DIDT setup", nullptr, kGcCacDidtPattern, sizeof(kGcCacDidtPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcCacDidt), orgGcCacDidt, false},
            {"CGTS timings", "_gc_apply_medium_grain_clock_gating_workaround_10_1", kGcCgtsPattern,
                sizeof(kGcCgtsPattern), reinterpret_cast<mach_vm_address_t>(wrapGcCgts),
                orgGcCgts, false},
            {"power features", "_gc_control_power_features_10_1", kGcPowerFeaturesPattern,
                sizeof(kGcPowerFeaturesPattern),
                reinterpret_cast<mach_vm_address_t>(wrapGcPowerFeatures), orgGcPowerFeatures, false},
            {"GC info", "_IpiBgmGetIpConfigFromDiscovery", kIpiBgmGetIpConfigPattern, sizeof(kIpiBgmGetIpConfigPattern),
                reinterpret_cast<mach_vm_address_t>(wrapIpiBgmGetIpConfig), orgIpiBgmGetIpConfig, false},
        };
        mach_vm_address_t at[arrsize(hooks)] {};
        for (size_t i = 0; i < arrsize(hooks); i++) {
            at[i] = findFunction(patcher, id, hooks[i].symbol, hooks[i].pattern, hooks[i].length, slide, size);
            if (at[i] == 0) {
                BCLOG("BC250HWL", "level 18: GC %s not found", hooks[i].name);
                if (hooks[i].guard) { return false; }
            }
        }
        for (size_t i = 0; i < arrsize(hooks); i++) {
            if (at[i] == 0) { continue; }
            KernelPatcher::RouteRequest request {nullptr, hooks[i].wrapper, hooks[i].org};
            request.from = at[i];
            if (!patcher.routeMultiple(id, &request, 1)) {
                BCLOG("BC250HWL", "level 18: GC %s failed to route", hooks[i].name);
                patcher.clearError();
                if (hooks[i].guard) { return false; }
            }
        }
        return true;
    }

    // Level 20: SDMA's hw_init as a dry run. sdma.c's hw_init (0xc363ceb: sdma, input, output) fills the queue
    // interface, checks the doorbell range and runs sdma_init_hw_internal (0xc365086): golden settings, the engine
    // start (+0x2f0, SDMA 5.0's 0xc365951) once, the MGCG/MGLS updates (+0x2d0/+0x2d8) and a restart of the queues
    // already created (none at boot). The engine start is Linux's sdma_v5_0_start for PSP-loaded microcode (SDMA_CNTL
    // UTC_L1_ENABLE and AUTO_CTXSW_ENABLE, UTCL1_CNTL RESP_MODE/REDO_DELAY, UTCL1_PAGE cache policy, F32_CNTL HALT
    // cleared), but first waits for the RLC autoload's bootload-complete bit (sdma_5_0_check_ucode_loaded, 0xc3658da:
    // RLC_RLCS_BOOTLOAD_STATUS bit 31), which never sets without autoload: Linux does not check, so the check reports
    // the microcode loaded. Linux's SDMA golden settings are applied with GC's (the Cyan Skillfish list includes
    // them), so Apple's Navi 10 SDMA list is not applied; SDMA 5.0.1 has no clock gating in Linux, so MGCG/MGLS are
    // skipped. SDMA reaches registers only through sdma_cgs.c (write 0xc36427a, write_ext2 0xc3642c6, read 0xc3641d8,
    // read_ext2 0xc364226: sdma, register, base index, [value,] hardware id (0x23 SDMA0, 0x24 SDMA1), [flag]).
    const UInt8 kSdmaWritePattern[]      = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x4F, 0x08, 0x49, 0x8B, 0x41, 0x38, 0x48,
             0x85, 0xC0, 0x74, 0x18, 0x41, 0x83, 0xF8, 0x03, 0x74, 0x06, 0x89, 0xD2, 0x03, 0x74, 0x97, 0x44, 0x49, 0x8B,
             0x79, 0x08, 0x89, 0xCA, 0x44, 0x89, 0xC1, 0x5D, 0xFF, 0xE0, 0x48, 0x8D, 0x15, 0x07};
    const UInt8 kSdmaWriteExt2Pattern[]  = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x57, 0x08, 0x49, 0x8B, 0x82, 0x20};
    const UInt8 kSdmaReadPattern[]       = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x47, 0x08, 0x49, 0x8B, 0x40, 0x40, 0x48,
              0x85, 0xC0, 0x74, 0x14, 0x83, 0xF9, 0x03, 0x74, 0x06, 0x89, 0xD2, 0x03, 0x74, 0x97, 0x44, 0x49, 0x8B, 0x78,
              0x08, 0x89, 0xCA, 0x5D, 0xFF, 0xE0, 0x48, 0x8D, 0x15, 0x62};
    const UInt8 kSdmaReadExt2Pattern[]   = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x4F, 0x08, 0x49, 0x8B, 0x81, 0x18};
    const UInt8 kSdmaGoldenPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC, 0x18, 0x48, 0xC7, 0x45, 0xE0,
            0x00, 0x00, 0x00, 0x00, 0x89, 0x75, 0xE8, 0x89, 0x55, 0xEC, 0x89, 0x4D, 0xF0, 0x48, 0x8B, 0x4F};
    const UInt8 kSdmaCheckUcodePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x48, 0x8D,
        0x35, 0x43, 0x00, 0x00, 0x00, 0x48, 0x89, 0xFA, 0xB9, 0x32, 0x00, 0x00, 0x00, 0xE8, 0x62};
    const UInt8 kSdmaMgcgPattern[]       = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x41, 0x89, 0xF6, 0x48, 0x89, 0xFB,
              0x8B, 0x47, 0x18, 0x83, 0xF8, 0x24, 0x74, 0x4C};
    const UInt8 kSdmaMglsPattern[]       = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x41, 0x89, 0xF6, 0x48, 0x89, 0xFB,
              0x8B, 0x47, 0x18, 0x83, 0xF8, 0x24, 0x74, 0x46};
    const UInt8 kSdmaHwInitPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x10, 0xC7, 0x45,
            0xEC, 0x00, 0x00, 0x00, 0x00, 0xB8};
    constexpr UInt32 kSdmaLogMax = 300;
    // sdma: +0x18 hardware id, +0x44 register bases by index, +0x278 SDMA_ID value, +0x27c/+0x27d MGCG/MGLS off,
    // +0x27e direct microcode load, +0x2b0 engine started, +0x2b1/+0x2b2 SR-IOV variants.
    constexpr UInt32 kSdmaHwId = 0x18, kSdmaBases = 0x44, kSdmaIdValue = 0x278, kSdmaDirectLoad = 0x27E;
    constexpr UInt32 kSdmaStarted = 0x2B0;

    mach_vm_address_t orgSdmaWrite = 0, orgSdmaWriteExt2 = 0, orgSdmaRead = 0, orgSdmaReadExt2 = 0, orgSdmaGolden = 0,
                      orgSdmaCheckUcode = 0, orgSdmaMgcg = 0, orgSdmaMgls = 0, orgSdmaHwInit = 0;
    bool   sdmaLive   = false;
    UInt32 sdmaWrites = 0, sdmaReads = 0;

    // The register the CGS layer reaches (base added unless the hardware id is 3).
    UInt32 sdmaAbs(void* sdma, UInt64 reg, UInt64 baseIndex, UInt64 hwId)
    {
        if (hwId == 3 || baseIndex > 3) { return static_cast<UInt32>(reg); }
        return static_cast<UInt32>(reg) + getMember<UInt32>(sdma, kSdmaBases + 4 * static_cast<UInt32>(baseIndex));
    }

    bool sdmaLinuxValue(void* sdma, UInt64 reg, UInt64 base, UInt64 hwId, UInt64 flag, UInt64& value);

    UInt64 wrapSdmaWrite(void* sdma, UInt64 reg, UInt64 base, UInt64 value, UInt64 hwId)
    {
        if (++sdmaWrites <= kSdmaLogMax) {
            BCLOG("BC250HWL", "SDMA%s: W 0x%05X = 0x%08llX (reg 0x%llX, base %llu, hw 0x%llX)", sdmaLive ? "" : " dry",
                sdmaAbs(sdma, reg, base, hwId), value & 0xFFFFFFFF, reg, base, hwId);
        }
        if (!sdmaLive) { return 0; }
        if (!sdmaLinuxValue(sdma, reg, base, hwId, 1, value)) { return 0; }
        return FunctionCast(wrapSdmaWrite, orgSdmaWrite)(sdma, reg, base, value, hwId);
    }

    // Level 21: the engine start's writes as Linux's sdma_v5_0_gfx_resume makes them (the engine start is the only
    // SDMA code that writes these registers at hw_init). Register offsets are SDMA0's; SDMA1's are 0x600 higher.
    constexpr UInt32 kSdmaInstanceStride = 0x600, kSdmaCntl = 0x1C, kSdmaId = 0x34, kSdmaUtcl1Cntl = 0x3C;
    constexpr UInt32 kSdmaUtcl1Page = 0x48;
    constexpr UInt32 kSdmaMidcmdPreempt = 1U << 5, kSdmaRespModeMask = 0xE00, kSdmaRespModeLinux = 3U << 9;
    constexpr UInt32 kSdmaPolicyMask = 0xFF0FFF, kSdmaPolicyLinux = (3U << 12) | (3U << 14);    // L2 DEFAULT (3).
    bool sdmaInHwInit = false;

    UInt32 wrapSdmaReadExt2(void* sdma, UInt64 reg, UInt64 base, UInt64 hwId, UInt64 flag);

    // Returns false if the write is dropped.
    bool sdmaLinuxValue(void* sdma, UInt64 reg, UInt64 base, UInt64 hwId, UInt64 flag, UInt64& value)
    {
        if (!sdmaInHwInit || base != 0) { return true; }
        const UInt32 rel = static_cast<UInt32>(reg) % kSdmaInstanceStride;
        const UInt32 old = static_cast<UInt32>(value);
        switch (rel) {
            case kSdmaId:
                BCLOG("BC250HWL", "SDMA: SDMA_ID write (0x%08X) dropped (Linux writes none)", old);
                return false;
            case kSdmaCntl:
                value = old | kSdmaMidcmdPreempt;
                break;
            case kSdmaUtcl1Cntl:
                value = (old & ~kSdmaRespModeMask) | kSdmaRespModeLinux;
                break;
            case kSdmaUtcl1Page: {
                const UInt32 current =
                    FunctionCast(wrapSdmaReadExt2, orgSdmaReadExt2)(sdma, reg, base, hwId, flag);
                value = (current & kSdmaPolicyMask) | kSdmaPolicyLinux;
                break;
            }
            default:
                return true;
        }
        BCLOG("BC250HWL", "SDMA: reg 0x%llX: Apple's 0x%08X written as Linux's 0x%08llX", reg, old, value);
        return true;
    }

    UInt64 wrapSdmaWriteExt2(void* sdma, UInt64 reg, UInt64 base, UInt64 value, UInt64 hwId, UInt64 flag)
    {
        if (++sdmaWrites <= kSdmaLogMax) {
            BCLOG("BC250HWL", "SDMA%s: W ext2 0x%05X = 0x%08llX (reg 0x%llX, base %llu, hw 0x%llX, 0x%llX)",
                sdmaLive ? "" : " dry", sdmaAbs(sdma, reg, base, hwId), value & 0xFFFFFFFF, reg, base, hwId, flag);
        }
        if (!sdmaLive) { return 0; }
        if (!sdmaLinuxValue(sdma, reg, base, hwId, flag, value)) { return 0; }
        return FunctionCast(wrapSdmaWriteExt2, orgSdmaWriteExt2)(sdma, reg, base, value, hwId, flag);
    }

    UInt32 wrapSdmaRead(void* sdma, UInt64 reg, UInt64 base, UInt64 hwId)
    {
        const auto value = FunctionCast(wrapSdmaRead, orgSdmaRead)(sdma, reg, base, hwId);
        if (++sdmaReads <= kSdmaLogMax) {
            BCLOG("BC250HWL", "SDMA: R 0x%05X -> 0x%08X (reg 0x%llX, base %llu, hw 0x%llX)",
                sdmaAbs(sdma, reg, base, hwId), value, reg, base, hwId);
        }
        return value;
    }

    UInt32 wrapSdmaReadExt2(void* sdma, UInt64 reg, UInt64 base, UInt64 hwId, UInt64 flag)
    {
        const auto value = FunctionCast(wrapSdmaReadExt2, orgSdmaReadExt2)(sdma, reg, base, hwId, flag);
        if (++sdmaReads <= kSdmaLogMax) {
            BCLOG("BC250HWL", "SDMA: R ext2 0x%05X -> 0x%08X (reg 0x%llX, base %llu, hw 0x%llX, 0x%llX)",
                sdmaAbs(sdma, reg, base, hwId), value, reg, base, hwId, flag);
        }
        return value;
    }

    void* wrapSdmaGolden(void* sdma, UInt32 type, UInt32 major, UInt32 minor)
    {
        const auto table = FunctionCast(wrapSdmaGolden, orgSdmaGolden)(sdma, type, major, minor);
        BCLOG("BC250HWL", "SDMA golden settings for 0x%02X %u.%u: Apple's (Navi 10) %p not applied (Linux's Cyan "
                          "Skillfish SDMA settings came with GC's list)",
            type, major, minor, table);
        return nullptr;
    }

    UInt32 wrapSdmaCheckUcode(void* sdma)
    {
        BCLOG("BC250HWL", "SDMA hw 0x%X: autoload bootload-complete wait skipped, microcode reported loaded (PSP loaded "
                          "it; Linux does not check)",
            getMember<UInt32>(sdma, kSdmaHwId));
        return 0;    // The wait's status: 0 is done (the engine start goes on only on 0).
    }

    void wrapSdmaMgcg(void* sdma, UInt32 enable)
    {
        BCLOG("BC250HWL", "SDMA hw 0x%X: MGCG update (enable %u) skipped (none on SDMA 5.0.1 in Linux)",
            getMember<UInt32>(sdma, kSdmaHwId), enable);
    }

    void wrapSdmaMgls(void* sdma, UInt32 enable)
    {
        BCLOG("BC250HWL", "SDMA hw 0x%X: MGLS update (enable %u) skipped (none on SDMA 5.0.1 in Linux)",
            getMember<UInt32>(sdma, kSdmaHwId), enable);
    }

    UInt32 wrapSdmaHwInit(void* sdma, void* input, void* output)
    {
        BCLOG("BC250HWL", "SDMA hw_init (%s) >>> hw 0x%X, bases 0x%X 0x%X 0x%X 0x%X, SDMA_ID value 0x%X, MGCG/MGLS off "
                          "%u/%u, direct load %u, started %u, SR-IOV %u/%u",
            sdmaLive ? "live" : "dry run", getMember<UInt32>(sdma, kSdmaHwId), getMember<UInt32>(sdma, kSdmaBases),
            getMember<UInt32>(sdma, kSdmaBases + 4), getMember<UInt32>(sdma, kSdmaBases + 8),
            getMember<UInt32>(sdma, kSdmaBases + 12), getMember<UInt32>(sdma, kSdmaIdValue),
            getMember<UInt8>(sdma, 0x27C), getMember<UInt8>(sdma, 0x27D), getMember<UInt8>(sdma, kSdmaDirectLoad),
            getMember<UInt8>(sdma, kSdmaStarted), getMember<UInt8>(sdma, 0x2B1), getMember<UInt8>(sdma, 0x2B2));
        if (getMember<UInt8>(sdma, kSdmaDirectLoad) != 0) {
            // Direct loading would write Apple's Navi 10 SDMA microcode through SDMA0_UCODE_ADDR/DATA.
            getMember<UInt8>(sdma, kSdmaDirectLoad) = 0;
            BCLOG("BC250HWL", "SDMA: direct microcode load turned off (PSP loaded the BC-250's)");
        }
        sdmaInHwInit   = true;
        const auto ret = FunctionCast(wrapSdmaHwInit, orgSdmaHwInit)(sdma, input, output);
        sdmaInHwInit   = false;
        BCLOG("BC250HWL", "SDMA hw_init (%s) <<< 0x%X after %u write(s), %u read(s)", sdmaLive ? "live" : "dry run", ret,
            sdmaWrites, sdmaReads);
        if (sdmaLive) {
            // The engine state as Linux's ring test would find it: F32 running, status.
            const UInt32 hw = getMember<UInt32>(sdma, kSdmaHwId);
            const UInt32 off = hw == 0x24 ? kSdmaInstanceStride : 0;
            BCLOG("BC250HWL", "SDMA hw 0x%X after start: F32_CNTL 0x%08X, SDMA_CNTL 0x%08X, STATUS 0x%08X, UTCL1_CNTL "
                              "0x%08X, UTCL1_PAGE 0x%08X",
                hw, wrapSdmaReadExt2(sdma, off + 0x2A, 0, hw, 1), wrapSdmaReadExt2(sdma, off + kSdmaCntl, 0, hw, 1),
                wrapSdmaReadExt2(sdma, off + 0x25, 0, hw, 1), wrapSdmaReadExt2(sdma, off + kSdmaUtcl1Cntl, 0, hw, 1),
                wrapSdmaReadExt2(sdma, off + kSdmaUtcl1Page, 0, hw, 1));
        }
        return ret;
    }

    // Returns false if any hook is missing; SDMA's hw_init must then not run (each one is a difference from Linux,
    // and the hw_init wrapper turns off direct loading).
    bool hookSdmaDryRun(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        struct Hook
        {
            const char*        name;
            const char*        symbol;
            const UInt8*       pattern;
            size_t             length;
            mach_vm_address_t  wrapper;
            mach_vm_address_t& org;
        };
        Hook hooks[] = {
            {"write", "_sdma_cgs_write_register", kSdmaWritePattern, sizeof(kSdmaWritePattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaWrite),
                orgSdmaWrite},
            {"write_ext2", "_sdma_cgs_write_register_ext2", kSdmaWriteExt2Pattern, sizeof(kSdmaWriteExt2Pattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaWriteExt2), orgSdmaWriteExt2},
            {"read", "_sdma_cgs_read_register", kSdmaReadPattern, sizeof(kSdmaReadPattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaRead),
                orgSdmaRead},
            {"read_ext2", "_sdma_cgs_read_register_ext2", kSdmaReadExt2Pattern, sizeof(kSdmaReadExt2Pattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaReadExt2), orgSdmaReadExt2},
            {"golden query", "_sdma_cgs_query_golden_settings", kSdmaGoldenPattern, sizeof(kSdmaGoldenPattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaGolden), orgSdmaGolden},
            {"microcode check", "_sdma_5_0_check_ucode_loaded", kSdmaCheckUcodePattern, sizeof(kSdmaCheckUcodePattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaCheckUcode), orgSdmaCheckUcode},
            {"MGCG", "_sdma_5_0_update_medium_grain_clock_gating", kSdmaMgcgPattern, sizeof(kSdmaMgcgPattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaMgcg),
                orgSdmaMgcg},
            {"MGLS", "_sdma_5_0_update_medium_grain_light_sleep", kSdmaMglsPattern, sizeof(kSdmaMglsPattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaMgls),
                orgSdmaMgls},
            {"hw_init", "_sdma_hw_init", kSdmaHwInitPattern, sizeof(kSdmaHwInitPattern),
                reinterpret_cast<mach_vm_address_t>(wrapSdmaHwInit), orgSdmaHwInit},
        };
        mach_vm_address_t at[arrsize(hooks)] {};
        for (size_t i = 0; i < arrsize(hooks); i++) {
            at[i] = findFunction(patcher, id, hooks[i].symbol, hooks[i].pattern, hooks[i].length, slide, size);
            if (at[i] == 0) {
                BCLOG("BC250HWL", "level 20: SDMA %s not found", hooks[i].name);
                return false;
            }
        }
        for (size_t i = 0; i < arrsize(hooks); i++) {
            KernelPatcher::RouteRequest request {nullptr, hooks[i].wrapper, hooks[i].org};
            request.from = at[i];
            if (!patcher.routeMultiple(id, &request, 1)) {
                BCLOG("BC250HWL", "level 20: SDMA %s failed to route", hooks[i].name);
                patcher.clearError();
                return false;
            }
        }
        return true;
    }

    // Level 22: once every SWIP is up, TTL's post-init (0xc3bbb80) publishes firmware versions (0xc3bc060) through the
    // IPI query router (0xc3a0731: context, {type, instance}, output; returns 0 answered, 2 bad arguments, 4 not
    // available). Type 0xC goes to VCN's IPI query (0xc3b385c), which walks VCN's IPI context; VCN is a stub whose
    // code never ran, so the context is empty and the query calls a null pointer (the first level 22 run panicked
    // there). Queries for a stubbed VCN are answered "not available"; every query is logged.
    const UInt8 kIpiQueryPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0xB8, 0x02, 0x00, 0x00,
        0x00, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0xC4};
    constexpr UInt32 kIpiQueryVcn = 0xC, kIpiQueryNotAvailable = 4, kSwipVcn = 9, kIpiQueryLogMax = 64;

    mach_vm_address_t orgIpiQuery = 0;
    UInt32            ipiQueries  = 0;

    UInt32 wrapIpiQuery(void* context, const UInt32* query, void* output)
    {
        const bool log = ++ipiQueries <= kIpiQueryLogMax;
        if (query != nullptr && query[0] == kIpiQueryVcn && swipStubbed(kSwipVcn)) {
            if (log) { BCLOG("BC250HWL", "IPI query 0x%X (instance %u): VCN is a stub, not available", query[0], query[1]); }
            return kIpiQueryNotAvailable;
        }
        const auto ret = FunctionCast(wrapIpiQuery, orgIpiQuery)(context, query, output);
        if (log && query != nullptr) {
            BCLOG("BC250HWL", "IPI query 0x%X (instance %u) <<< 0x%X", query[0], query[1], ret);
        }
        return ret;
    }

    // Level 22: the query router is one of many: TTL's post-init also reaches VCN through other IPI routers (the
    // second level 22 run panicked in 0xc39e519 -> 0xc3b374e), 24 call sites in all, each fetching VCN's IPI context
    // with 0xc3b2601 (ipi -> *(ipi + 8 + 9 * 8)). The context exists (IpiInitializeIpInterfaces made it, with the
    // instance count from IP discovery) but its per-instance entries were never filled, VCN's code never having run.
    // Every VCN IPI function returns early on a null context, so while VCN is a stub its getter returns null; the
    // context itself stays in the IPI table for teardown. (JPEG's IPI functions check a handle only its sw_init sets,
    // so the stubbed JPEG is already answered as absent.)
    const UInt8 kIpiVcnContextPattern[] = {0x55, 0x48, 0x89, 0xE5, 0xBE, 0x09, 0x00, 0x00, 0x00, 0x5D, 0xE9, 0xC0, 0x34,
        0xFF, 0xFF};
    UInt32 ipiVcnNulls = 0;

    void* wrapIpiVcnContext(void* ipi)
    {
        if (swipStubbed(kSwipVcn)) {
            if (++ipiVcnNulls <= 8) { BCLOG("BC250HWL", "IPI: VCN context requested; VCN is a stub, none given"); }
            return nullptr;
        }
        return ipi != nullptr ? getMember<void*>(ipi, 8 + kSwipVcn * 8) : nullptr;
    }

    // Level 27: the accelerator's writes are made. The hubs take page-table bases (GCVM/MMVM_CONTEXTn_PAGE_TABLE_BASE_ADDR
    // _LO32/HI32) as physical addresses; the BC-250's FB is a carve-out at GCMC_VM_FB_OFFSET, so a base inside VRAM,
    // whether given as a VRAM offset (as GVM's) or as the FB's MC address (0xF4...), gets the offset (Linux:
    // vram_base_offset). The low half is held until its high half arrives, so the full address decides.
    UInt32 accelPdbPendingReg = 0, accelPdbPendingValue = 0, accelPdbAdjusted = 0;

    bool accelIsPdb(UInt32 reg)
    {
        return (reg >= kGcContext0Base && reg < kGcContext0Base + 2 * kGvmContexts) ||
               (reg >= kMmContext0Base && reg < kMmContext0Base + 2 * kGvmContexts);
    }

    // Returns true if the write was handled (held, or written as an adjusted pair).
    bool accelPdbWrite(void* regs, UInt32 reg, UInt32 value)
    {
        const auto base   = reg >= kMmContext0Base ? kMmContext0Base : kGcContext0Base;
        const bool isLow  = ((reg - base) & 1) == 0;
        auto       write  = [regs](UInt32 r, UInt32 v) { FunctionCast(wrapAccelRegWrite, orgAccelRegWrite)(regs, r, v); };
        if (isLow) {
            if (accelPdbPendingReg != 0) {
                BCLOG("BC250HWL", "Accel: PDB low 0x%05X = 0x%08X written without its high half", accelPdbPendingReg,
                    accelPdbPendingValue);
                write(accelPdbPendingReg, accelPdbPendingValue);
            }
            accelPdbPendingReg   = reg;
            accelPdbPendingValue = value;
            return true;
        }
        if (accelPdbPendingReg != reg - 1) {
            BCLOG("BC250HWL", "Accel: PDB high 0x%05X = 0x%08X without its low half, as given", reg, value);
            return false;
        }
        const UInt32 lo      = accelPdbPendingValue;
        accelPdbPendingReg   = 0;
        const UInt64 given   = (static_cast<UInt64>(value) << 32) | lo;
        const UInt64 address = given & ~0xFFFULL;
        UInt64       phys    = address;
        if (address < gvmFbSize) {
            phys = address + gvmFbOffset;
        }
        else if (address >= gvmFbBase && address < gvmFbBase + gvmFbSize) {
            phys = address - gvmFbBase + gvmFbOffset;
        }
        const UInt64 result = phys | (given & 0xFFF);
        if (phys != address) { accelPdbAdjusted++; }
        BCLOG("BC250HWL", "Accel: PDB 0x%05X/0x%05X = 0x%llX -> 0x%llX", reg - 1, reg, given, result);
        write(reg - 1, static_cast<UInt32>(result));
        write(reg, static_cast<UInt32>(result >> 32));
        return true;
    }

    bool accelWritesLive() { return accelLive && gvmLive && gvmFbOffset != 0; }

    // Level 28: the accelerator's own page tables (per-process VMIDs). AMDGFX10VMM::getPDEValue(level, address) and
    // getPTEValue(level, address, flags, fragment) mask the address in and add flags only; both the CPU and the SDMA
    // (updateContiguousPTEsWithDMAUsingAddr) paths take their entries from them (VMM vtable +0x1D8/+0x1E0). A PDE
    // always points to a page-table block in VRAM, a PTE without SYSTEM (bit 1) to VRAM, and the hubs take both as
    // physical addresses: on the BC-250 VRAM is the carve-out at GCMC_VM_FB_OFFSET, so entries built from VRAM
    // offsets pointed the GPU at system memory 0-512 MB (the first Metal work after the level 28 clocks hung the
    // machine: IPI timeouts ~32 s after boot). Linux adds vram_base_offset (gmc_v10_0_get_vm_pde, amdgpu_vm PTEs).
    // A VMID's page-table base comes from prepareVMInvalidateRequest (info +0x18 -> request +4/+0xC), used by the
    // MMIO path (accelPdbWrite) and the in-ring SDMA VM program packet alike. bc250pte=0 keeps Apple's entries.
    mach_vm_address_t orgGetPdeValue = 0, orgGetPteValue = 0, orgDecodePte = 0, orgPrepareVmInvalidate = 0;
    UInt32            vmPdeLogged = 0, vmPteLogged = 0, vmPdbLogged = 0, vmEntriesAdjusted = 0;
    constexpr UInt32  kVmEntryLogMax = 12;
    constexpr UInt64  kPteSystem = 1ULL << 1, kPteAddrMask = 0x0000FFFFFFFFF000ULL, kPdeAddrMask = 0x0000FFFFFFFFFFC0ULL;

    // A VRAM address (offset or MC) as the carve-out's physical address; anything else is returned as given.
    UInt64 vramPhys(UInt64 address)
    {
        if (address < gvmFbSize) { return address + gvmFbOffset; }
        if (address >= gvmFbBase && address < gvmFbBase + gvmFbSize) { return address - gvmFbBase + gvmFbOffset; }
        return address;
    }

    UInt64 vmEntryAdjust(UInt64 entry, UInt64 mask)
    {
        const UInt64 address = entry & mask;
        const UInt64 phys    = vramPhys(address);
        if (phys == address) { return entry; }
        vmEntriesAdjusted++;
        return (entry & ~mask) | (phys & mask);
    }

    UInt64 wrapGetPdeValue(void* vmm, UInt32 level, UInt64 address)
    {
        const auto given = FunctionCast(wrapGetPdeValue, orgGetPdeValue)(vmm, level, address);
        const auto entry = vmEntryAdjust(given, kPdeAddrMask);
        if (++vmPdeLogged <= kVmEntryLogMax) {
            BCLOG("BC250HWL", "VMM: PDE level %u, address 0x%llX: 0x%016llX -> 0x%016llX", level, address, given, entry);
        }
        return entry;
    }

    UInt64 wrapGetPteValue(void* vmm, UInt32 level, UInt64 address, UInt32 flags, UInt32 fragment)
    {
        const auto given = FunctionCast(wrapGetPteValue, orgGetPteValue)(vmm, level, address, flags, fragment);
        const auto entry = (given & kPteSystem) != 0 ? given : vmEntryAdjust(given, kPteAddrMask);
        if (++vmPteLogged <= kVmEntryLogMax) {
            BCLOG("BC250HWL", "VMM: PTE level %u, address 0x%llX, flags 0x%X, fragment %u: 0x%016llX -> 0x%016llX", level,
                address, flags, fragment, given, entry);
        }
        return entry;
    }

    // The inverse for Apple's readers of entries: a VRAM entry's physical address back to a VRAM offset.
    bool wrapDecodePte(void* vmm, UInt64 entry, UInt64* address, bool* system)
    {
        const bool valid = FunctionCast(wrapDecodePte, orgDecodePte)(vmm, entry, address, system);
        if (valid && address != nullptr && system != nullptr && !*system && *address >= gvmFbOffset &&
            *address < gvmFbOffset + gvmFbSize)
        {
            *address -= gvmFbOffset;
        }
        return valid;
    }

    // The SDMA path (mapVMPT -> updateContiguousPTEsWithDMAUsingAddr(this, pe, count, addr, flags, incr)) passes the
    // entries' address separately: the PTE/PDE value above is only the flag template (built with address 0), and the
    // SDMA PTEPDE packet writes (addr + i * incr) | flags (as Linux's sdma_v5_0_vm_set_pte_pde). The FB offset
    // therefore belongs in addr: OR-ed in through the template it lost bit 28 for VRAM offsets from 256 MB (the first
    // Metal page tables sat at 0x1B40B000 and were pointed 256 MB low; the GPU hung on them, then the machine). The
    // template's base is taken back out and a VRAM addr (template without SYSTEM) is translated.
    mach_vm_address_t orgUpdatePtesDma = 0;
    UInt32            vmDmaLogged      = 0;

    UInt32 wrapUpdatePtesDma(void* ctx, UInt64 pe, UInt64 count, UInt64 addr, UInt64 flags, UInt64 incr)
    {
        const UInt64 givenAddr = addr, givenFlags = flags;
        const UInt64 base      = flags & kPteAddrMask;
        if (gvmFbOffset != 0 && base == gvmFbOffset) { flags &= ~kPteAddrMask; }
        if ((flags & kPteSystem) == 0) { addr = vramPhys(addr); }
        if (++vmDmaLogged <= kVmEntryLogMax * 2) {
            BCLOG("BC250HWL", "VMM: DMA entries at 0x%llX x%llu: addr 0x%llX -> 0x%llX, flags 0x%llX -> 0x%llX, incr 0x%llX",
                pe, count, givenAddr, addr, givenFlags, flags, incr);
        }
        return FunctionCast(wrapUpdatePtesDma, orgUpdatePtesDma)(ctx, pe, count, addr, flags, incr);
    }

    // 28z2: the CP-scheduled queues' VMIDs (8-15) get their page-table base from the HIQ's MAP_PROCESS packet
    // (AMDGFX10HIQHWChannel::fillMapProcessPacket, header 0xC00EA100): dwords 2-3 take the VM context's root page
    // table as a VRAM offset (VMM vtable +0x200), untranslated, and the CP firmware writes it to the VMID's
    // PAGE_TABLE_BASE. 28z: VMID 10 walked "page tables" at system address 0x1B41C000 (the same root was translated
    // for VMID 3 through prepareVMInvalidateRequest), faulted on its first read (green screen) and the machine froze.
    mach_vm_address_t orgFillMapProcess = 0;
    UInt32            vmMapProcessLogged = 0;

    UInt32* wrapFillMapProcess(void* channel, UInt32* packet, UInt64 context, UInt64 tba, UInt32 a, UInt32 b, UInt32 c)
    {
        auto* end = FunctionCast(wrapFillMapProcess, orgFillMapProcess)(channel, packet, context, tba, a, b, c);
        if (packet == nullptr) { return end; }
        const UInt64 given = (static_cast<UInt64>(packet[3]) << 32) | packet[2];
        const UInt64 base  = vramPhys(given & ~0xFFFULL) | (given & 0xFFF);
        packet[2]          = static_cast<UInt32>(base);
        packet[3]          = static_cast<UInt32>(base >> 32);
        if (++vmMapProcessLogged <= kVmEntryLogMax) {
            BCLOG("BC250HWL", "VMM: MAP_PROCESS (HIQ): page-table base 0x%llX -> 0x%llX (ordinal 2 0x%08X)", given, base,
                packet[1]);
        }
        return end;
    }

    // 28z6: page-table blocks the CPU writes. AMDHWVMContext::prepareVmBlockForUpdate asks for CPU access to a block
    // (prepareVmBlockForCPUAccess(address, size, &map)), and for VRAM Apple's memory manager maps the range through
    // the BAR (vtable +0x208). The BC-250's BAR0 covers 256 MB of the 512 MB carve-out and the page tables sit at
    // ~436 MB: a Firefox GPU helper's write to an allocated VA (0x400004000) faulted on a PTB entry that was never
    // written (0), while the SDMA-written entries next to it were right. The carve-out is system RAM, so a VRAM block
    // past the BAR is mapped for the CPU at its physical address (uncached), and released again by
    // completeVmBlockForCPUAccess. bc250ptcpu=0 keeps Apple's mapping.
    mach_vm_address_t orgPrepareCpuAccess = 0, orgCompleteCpuAccess = 0;
    constexpr UInt64  kBc250BarSize       = 0x10000000;
    IOMemoryMap*      ownCpuMaps[32]      = {};
    UInt32            cpuAccessLogged = 0, cpuAccessOwn = 0;

    void* wrapPrepareCpuAccess(void* context, UInt64 address, UInt64 size, IOMemoryMap** map)
    {
        // The block's address is an MC address (0xF4...) or a VRAM offset.
        const bool   mc      = gvmFbBase != 0 && address >= gvmFbBase && address < gvmFbBase + gvmFbSize;
        const UInt64 offset  = mc ? address - gvmFbBase : address;
        const bool   pastBar = gvmFbSize != 0 && (mc || address < gvmFbSize) && offset >= kBc250BarSize && size != 0 &&
                             offset + size <= gvmFbSize;
        if (!pastBar) {
            void* ret = FunctionCast(wrapPrepareCpuAccess, orgPrepareCpuAccess)(context, address, size, map);
            if (++cpuAccessLogged <= 8) {
                BCLOG("BC250HWL", "VM block CPU access 0x%llX + 0x%llX (Apple's) -> %p", address, size, ret);
            }
            return ret;
        }
        const UInt64 phys = offset + gvmFbOffset;
        auto*        md   = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(phys),
                     static_cast<IOByteCount>(size), kIODirectionInOut);
        IOMemoryMap* own  = md != nullptr ? md->map(kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(md);
        void* ret = own != nullptr ? reinterpret_cast<void*>(own->getVirtualAddress()) : nullptr;
        if (own != nullptr) {
            for (auto& slot : ownCpuMaps) {
                if (slot == nullptr) {
                    slot = own;
                    break;
                }
            }
        }
        if (map != nullptr) { *map = own; }
        if (++cpuAccessOwn <= 16) {
            BCLOG("BC250HWL", "VM block CPU access 0x%llX + 0x%llX past the BAR: carve-out 0x%llX mapped at %p", address,
                size, phys, ret);
        }
        return ret;
    }

    void wrapCompleteCpuAccess(void* context, UInt64 address, IOMemoryMap** map)
    {
        if (map != nullptr && *map != nullptr) {
            for (auto& slot : ownCpuMaps) {
                if (slot != nullptr && slot == *map) {
                    __asm__ volatile("mfence" ::: "memory");
                    slot->release();
                    slot = nullptr;
                    *map = nullptr;
                    return;
                }
            }
        }
        FunctionCast(wrapCompleteCpuAccess, orgCompleteCpuAccess)(context, address, map);
    }

    // 28z9: all of VRAM CPU-visible, as Linux's amdgpu does on APUs (aper_base = the FB offset, aper_size = VRAM).
    // Apple's AMDHWMemory keys everything off its CPU-visible size (+0x48, BAR0's 256 MB): CPU-visible (type 0)
    // allocations must fit below it, so every CPU-mapped buffer (IOSurfaces, uploads) shares 256 MB while ~3.75 GB
    // above it stays unused ("AMD ERROR! Failed to allocate ... 16-40 MB free memory remaining, and ~4 GB fixed-free"
    // at 4K, then a green screen), and CPU access above it windows VRAM into the BAR by reprogramming HDP registers
    // (prepareForCPUAccess, AMDGFX10HWMemory::prepare). The carve-out is system RAM the CPU reaches directly, so the
    // visible size is raised to the whole VRAM (hasInvisibleVRAM() is then false, as on a GPU with a resizable BAR)
    // and the virtual space mapOffset maps through (BAR0's descriptor) becomes the carve-out's physical range. Such a
    // range is not managed RAM, so default mappings of it are uncached, as BAR mappings are. bc250fullvram=0 keeps
    // Apple's 256 MB layout.
    mach_vm_address_t   orgSetVirtualSpace = 0;
    IOMemoryDescriptor* carveOutSpace      = nullptr;
    bool                fullVram = false, fullVramActive = false;

    void wrapSetVirtualSpace(void* memory, IOMemoryDescriptor* space)
    {
        IOMemoryDescriptor* given = space;
        if (fullVramActive && space != nullptr) { space = carveOutSpace; }
        BCLOG("BC250HWL", "HWMemory::setVirtualSpace %p (%llu MiB) -> %p (%llu MiB)", given,
            given != nullptr ? static_cast<UInt64>(given->getLength()) >> 20 : 0, space,
            space != nullptr ? static_cast<UInt64>(space->getLength()) >> 20 : 0);
        FunctionCast(wrapSetVirtualSpace, orgSetVirtualSpace)(memory, space);
    }

    void raiseVisibleVram(void* memory)
    {
        if (!fullVram || fullVramActive) { return; }
        auto&        total   = getMember<UInt64>(memory, 0x40);
        auto&        visible = getMember<UInt64>(memory, 0x48);
        const UInt64 bar     = visible;
        if (!gvmLive || total == 0 || bar >= total || total > gvmFbSize) {
            BCLOG("BC250HWL", "full VRAM: not applied (GVM %s, VRAM 0x%llX, visible 0x%llX, carve-out 0x%llX)",
                gvmLive ? "live" : "dry", total, bar, gvmFbSize);
            return;
        }
        if (carveOutSpace == nullptr) {
            carveOutSpace = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(gvmFbOffset),
                static_cast<IOByteCount>(total), kIODirectionInOut);
        }
        if (carveOutSpace == nullptr) {
            BCLOG("BC250HWL", "full VRAM: no descriptor for carve-out 0x%llX + 0x%llX; not applied", gvmFbOffset, total);
            return;
        }
        visible        = total;
        fullVramActive = true;
        // If the BAR's descriptor is already the virtual space, it is replaced now; later calls are swapped above.
        auto* space = getMember<IOMemoryDescriptor*>(memory, 0xA8);
        if (space != nullptr && space != carveOutSpace) { wrapSetVirtualSpace(memory, space); }
        BCLOG("BC250HWL", "full VRAM: CPU-visible 0x%llX -> 0x%llX, CPU access through carve-out 0x%llX", bar, visible,
            gvmFbOffset);
    }

    void wrapPrepareVmInvalidate(void* vmm, UInt32* request, const UInt8* info, bool alt)
    {
        FunctionCast(wrapPrepareVmInvalidate, orgPrepareVmInvalidate)(vmm, request, info, alt);
        if (request == nullptr || info == nullptr || info[0x24] == 0) { return; }
        const UInt64 given = (static_cast<UInt64>(request[3]) << 32) | request[1];
        const UInt64 base  = vramPhys(given & ~0xFFFULL) | (given & 0xFFF);
        request[1]         = static_cast<UInt32>(base);
        request[3]         = static_cast<UInt32>(base >> 32);
        if (++vmPdbLogged <= kVmEntryLogMax) {
            BCLOG("BC250HWL", "VMM: VM program (hub %u, VMID %u): PDB reg 0x%X/0x%X = 0x%llX -> 0x%llX",
                getMember<UInt32>(const_cast<UInt8*>(info), 0), getMember<UInt32>(const_cast<UInt8*>(info), 4),
                request[0], request[2], given, base);
        }
    }

    // Level 28: AMDAccelDevice::getHardwareInfo (0xbe619de) copies the hardware info (0x204 bytes, hardware +0xD0) for
    // the Metal driver and returns kIOReturnError when a clock (+0xC0 reference, +0xC8 system, +0xD0 memory, +0xD8 CG
    // reference) is 0, or +0x0 (16 bits), +0x18 or +0x20 is. Its copy gets the BC-250's clocks and the call succeeds
    // when only the clocks failed (bc250clk=1); the hardware object keeps its zeros.
    constexpr UInt32  kInfoRefClk = 0xC0, kInfoSysClk = 0xC8, kInfoMemClk = 0xD0, kInfoCgRefClk = 0xD8;
    constexpr UInt32  kIOReturnErrorCode = 0xE00002BC;
    mach_vm_address_t orgAccelHwInfo = 0;
    UInt32            accelHwInfoCalls = 0;

    UInt32 wrapAccelHwInfo(void* device, UInt8* out)
    {
        auto ret = FunctionCast(wrapAccelHwInfo, orgAccelHwInfo)(device, out);
        if (out == nullptr || hwInfoClkMode() != 1) { return ret; }
        auto& ref = getMember<UInt64>(out, kInfoRefClk);
        auto& sys = getMember<UInt64>(out, kInfoSysClk);
        auto& mem = getMember<UInt64>(out, kInfoMemClk);
        auto& cg  = getMember<UInt64>(out, kInfoCgRefClk);
        const bool otherValid =
            getMember<UInt16>(out, 0) != 0 && getMember<UInt64>(out, 0x18) != 0 && getMember<UInt64>(out, 0x20) != 0;
        const auto given = ret;
        if (ref == 0) { ref = kBc250RefClk; }
        if (sys == 0) { sys = kBc250SysClk; }
        if (mem == 0) { mem = kBc250MemClk; }
        if (cg == 0) { cg = kBc250RefClk; }
        if (ret == kIOReturnErrorCode && otherValid) { ret = 0; }
        if (++accelHwInfoCalls <= 16) {
            BCLOG("BC250HWL", "AccelDevice::getHardwareInfo <<< 0x%X (Apple's 0x%X), clocks %llu/%llu/%llu/%llu, device 0x%X, "
                              "+0x18 0x%llX, +0x20 0x%llX",
                ret, given, ref, sys, mem, cg, getMember<UInt16>(out, 0), getMember<UInt64>(out, 0x18),
                getMember<UInt64>(out, 0x20));
        }
        return ret;
    }

    // Level 28: the hubs' getValue_VM_INVALIDATE_ENG_REQ(vmids, flushType, pdeBits) builds every invalidation
    // request: vmids | flushType << 16 | L2 PTEs | L1 PTEs | PDE0-2 by pdeBits (Apple: flush type 1, PDE0 only,
    // 0x00990001). Level 27 rewrote the MMIO writes of it; with Metal running the requests also go into the SDMA/PM4
    // rings (VM program packets, prepareVMInvalidateRequest +0x44) as Apple's and are never acknowledged either: the
    // GPU stopped with the VM L2 busy (GCVM_L2_STATUS 1) and the fetcher waiting, eventTimeout came 10 s later, then
    // the machine froze (28h). At the source, every request is GVM's form, 0x02F80000 | vmids (bc250inv=0: Apple's).
    mach_vm_address_t orgHubInvReq[5] = {};
    UInt32            hubInvReqLogged = 0;

    template<size_t I>
    UInt32 wrapHubInvReq(void* hub, UInt32 vmids, UInt32 flushType, UInt32 pdeBits)
    {
        const auto given = FunctionCast(wrapHubInvReq<I>, orgHubInvReq[I])(hub, vmids, flushType, pdeBits);
        if (!accelInvalidateAsGvm()) { return given; }
        const auto req = kInvReqGvm | (given & 0xFFFF);
        if (++hubInvReqLogged <= 8) {
            BCLOG("BC250HWL", "Hub %zu: invalidate request (VMIDs 0x%X, flush type %u, PDE bits 0x%X) 0x%08X -> 0x%08X", I,
                vmids, flushType, pdeBits, given, req);
        }
        return req;
    }

    // Level 28: SDMA's in-ring invalidation, AMDGFX10SDMAChannel::writeVMInvalidateCommand(buf, range, vmids): for the
    // GC hub a GPUVM_INV packet (dword 0 = opcode 0x10; dword 1 = (request & 0xFFF80000) | vmids + 0x10000, i.e. flush
    // type 1 forced whatever the request; dwords 2-3 the range). The live dump of the hung GPU (28j) had SDMA1's page
    // queue parked on that opcode (STATUS2.CMD_OP 0x10, firmware looping) with the GC VM L2 busy: VMID 1's page-table
    // program queued behind it never landed (CTX1 PDB 0), and WindowServer's first GFX work waited on its fence until
    // the userspace watchdog panicked. Flush type 0, as the requests the hub acknowledges (GVM's 0x02F80000 form).
    //
    // With flush type 0 the packet still never completed (28k): SDMA 5.0's firmware on the BC-250 does not finish
    // GPUVM_INV at all (Linux's sdma_v5_0 never emits it; it invalidates with SRBM_WRITE of the REQ register and a
    // POLL_REGMEM of its ACK). Apple builds exactly that sequence for each hub in initializeVMInvalidateFrame (channel
    // +0x380 + hub * 0x200, from +0x54): for the GC hub 21 dwords at +0x3D4 = SRBM_WRITE range lo (value dword 2),
    // SRBM_WRITE range hi (5), SRBM_WRITE REQ (8), an optional dummy read of REQ, POLL_REGMEM ACK == mask (value 18,
    // mask 19); the MM hub's (+0x5D4, with a semaphore) is what writeVMInvalidateCommand already emits. The GC
    // GPUVM_INV (4 dwords) is replaced by the GC template (17 more; invalidateVM sizes its buffer with the buf == null
    // query and commits that size), with GVM's request. If the template does not look as expected, GPUVM_INV stays,
    // with flush type 0.
    constexpr UInt32  kSdmaGcInvTemplate = 0x3D4, kSdmaGcInvDwords = 21, kSdmaGpuvmInvDwords = 4;
    mach_vm_address_t orgSdmaVmInvalidate = 0;
    UInt32            sdmaVmInvLogged     = 0;

    const UInt32* sdmaGcInvTemplate(void* channel)
    {
        const auto* t = reinterpret_cast<const UInt32*>(static_cast<UInt8*>(channel) + kSdmaGcInvTemplate);
        const bool  ok = (t[0] & 0xFF) == 0x0E && (t[3] & 0xFF) == 0x0E && (t[6] & 0xFF) == 0x0E && (t[15] & 0xFF) == 0x08;
        return ok ? t : nullptr;
    }

    UInt32 wrapSdmaVmInvalidate(void* channel, UInt32* buf, const void* range, const UInt32* vmids)
    {
        const bool gc       = vmids != nullptr && vmids[0] != 0 && accelInvalidateAsGvm();
        const auto* tmpl    = gc && channel != nullptr ? sdmaGcInvTemplate(channel) : nullptr;
        const auto  extra   = tmpl != nullptr ? kSdmaGcInvDwords - kSdmaGpuvmInvDwords : 0;
        const auto  dwords  = FunctionCast(wrapSdmaVmInvalidate, orgSdmaVmInvalidate)(channel, buf, range, vmids);
        if (buf == nullptr) { return dwords + (dwords >= kSdmaGpuvmInvDwords ? extra : 0); }
        if (!gc || dwords < kSdmaGpuvmInvDwords || (buf[0] & 0xFF) != 0x10) { return dwords; }
        const UInt32 given = buf[1], lo = buf[2], hi = buf[3];
        if (tmpl == nullptr) {
            buf[1] &= ~0x00070000U;
            if (++sdmaVmInvLogged <= 8) {
                BCLOG("BC250HWL", "SDMA: GPUVM_INV (VMIDs 0x%X/0x%X) request 0x%08X -> 0x%08X (no GC template)", vmids[0],
                    vmids[1], given, buf[1]);
            }
            return dwords;
        }
        memmove(buf + kSdmaGcInvDwords, buf + kSdmaGpuvmInvDwords, (dwords - kSdmaGpuvmInvDwords) * sizeof(UInt32));
        memcpy(buf, tmpl, kSdmaGcInvDwords * sizeof(UInt32));
        buf[2]  = lo;
        buf[5]  = hi;
        buf[8]  = kInvReqGvm | (vmids[0] & 0xFFFF);
        buf[18] = vmids[0] & 0xFFFF;
        buf[19] = vmids[0] & 0xFFFF;
        if (++sdmaVmInvLogged <= 8) {
            BCLOG("BC250HWL", "SDMA: GPUVM_INV (VMIDs 0x%X/0x%X, 0x%08X) replaced by register writes: regs 0x%X/0x%X REQ 0x%X "
                              "= 0x%08X, poll ACK 0x%X == 0x%X, range 0x%08X 0x%08X, %u -> %u dwords",
                vmids[0], vmids[1], given, buf[1], buf[4], buf[7], buf[8], buf[16] >> 2, buf[18], lo, hi, dwords,
                dwords + extra);
        }
        return dwords + extra;
    }

    // Level 28: with the GC invalidation as register writes (28l) the GPU finished WindowServer's first work and went
    // idle (GFX ring 0x80 -> 0x180 consumed, L2 idle), yet AMDAccelChannel::waitForTimestamp kept polling. The stamps
    // come from memory: HWChannel::checkForTimestampUpdate compares submitted (+0x80) with last read (+0x84), and
    // timestampUpdated reads the fence through the CPU pointer at +0xC0. Logged per call (first 24, then every 4096th):
    // the stamps, the fence as the CPU sees it, and the fields around it.
    mach_vm_address_t orgCheckTimestamp = 0;
    UInt32            checkTimestampCalls = 0;
    void*             hwChannelsSeen[16] {};

    void hwChannelsDump(const char* when)
    {
        for (auto* ch : hwChannelsSeen) {
            if (ch == nullptr) { break; }
            const auto* fence = getMember<volatile UInt32*>(ch, 0xC0);
            BCLOG("BC250HWL", "HWChannel %p (%s) at %s: submitted %u, last read %u, fence %u, scheduler slot 0x%llX", ch,
                className(ch), when, getMember<UInt32>(ch, 0x80), getMember<UInt32>(ch, 0x84),
                fence != nullptr ? *fence : 0xDEADBEEF, getMember<UInt64>(ch, 0xE0));
        }
    }

    // AMDAccelChannel::waitForTimestamp(stamp): who waits for what (first 16 calls).
    mach_vm_address_t orgAccelWaitTimestamp = 0;
    UInt32            accelWaitTimestampCalls = 0;

    UInt32 wrapAccelWaitTimestamp(void* channel, UInt32 stamp)
    {
        if (++accelWaitTimestampCalls <= 16) {
            BCLOG("BC250HWL", "AccelChannel %p (%s)::waitForTimestamp(%u) >>> (+0x1a8 %u, +0x1d8 %u)", channel,
                className(channel), stamp, getMember<UInt32>(channel, 0x1A8), getMember<UInt32>(channel, 0x1D8));
            hwChannelsDump("wait start");
        }
        const auto ret = FunctionCast(wrapAccelWaitTimestamp, orgAccelWaitTimestamp)(channel, stamp);
        if (accelWaitTimestampCalls <= 16) {
            BCLOG("BC250HWL", "AccelChannel %p::waitForTimestamp(%u) <<< 0x%X", channel, stamp, ret);
        }
        return ret;
    }

    bool wrapCheckTimestamp(void* channel)
    {
        const bool ret = FunctionCast(wrapCheckTimestamp, orgCheckTimestamp)(channel);
        for (auto& seen : hwChannelsSeen) {
            if (seen == channel) { break; }
            if (seen == nullptr) {
                seen = channel;
                break;
            }
        }
        const auto n   = ++checkTimestampCalls;
        if (channel != nullptr && (n <= 24 || (n % 4096) == 0)) {
            const auto* fence = getMember<volatile UInt32*>(channel, 0xC0);
            BCLOG("BC250HWL", "HWChannel %p (%s) timestamps: submitted %u, last read %u, fence %u at %p; +0xB8 0x%llX +0xC8 "
                              "0x%llX +0xD0 0x%llX +0xE0 0x%llX (call %u, %d)",
                channel, className(channel), getMember<UInt32>(channel, 0x80), getMember<UInt32>(channel, 0x84),
                fence != nullptr ? *fence : 0xDEADBEEF, fence, getMember<UInt64>(channel, 0xB8),
                getMember<UInt64>(channel, 0xC8), getMember<UInt64>(channel, 0xD0), getMember<UInt64>(channel, 0xE0), n,
                ret);
        }
        return ret;
    }

    void hookHubInvalidateRequests(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        KernelPatcher::RouteRequest requests[] = {
            {"__ZN30AMDRadeonX6000_AMDGCHub_10_1_030getValue_VM_INVALIDATE_ENG_REQEjjj", wrapHubInvReq<0>,
                orgHubInvReq[0]},
            {"__ZN30AMDRadeonX6000_AMDGCHub_10_1_130getValue_VM_INVALIDATE_ENG_REQEjjj", wrapHubInvReq<1>,
                orgHubInvReq[1]},
            {"__ZN30AMDRadeonX6000_AMDGCHub_10_1_230getValue_VM_INVALIDATE_ENG_REQEjjj", wrapHubInvReq<2>,
                orgHubInvReq[2]},
            {"__ZN29AMDRadeonX6000_AMDMMHub_2_0_030getValue_VM_INVALIDATE_ENG_REQEjjj", wrapHubInvReq<3>,
                orgHubInvReq[3]},
            {"__ZN29AMDRadeonX6000_AMDMMHub_2_0_230getValue_VM_INVALIDATE_ENG_REQEjjj", wrapHubInvReq<4>,
                orgHubInvReq[4]},
            {"__ZN34AMDRadeonX6000_AMDGFX10SDMAChannel24writeVMInvalidateCommandEPjPK28AMD_VM_INVALIDATE_RANGE_INFOPKj",
                wrapSdmaVmInvalidate, orgSdmaVmInvalidate},
            {"__ZN27AMDRadeonX6000_AMDHWChannel23checkForTimestampUpdateEv", wrapCheckTimestamp, orgCheckTimestamp},
            {"__ZN30AMDRadeonX6000_AMDAccelChannel16waitForTimestampEj", wrapAccelWaitTimestamp, orgAccelWaitTimestamp},
        };
        if (patcher.routeMultiple(id, requests, arrsize(requests), slide, size)) {
            BCLOG("BC250HWL", "level 28: invalidation requests as GVM's at the source (all rings and MMIO)");
        }
        else {
            BCLOG("BC250HWL", "level 28: invalidation request hooks not installed");
            patcher.clearError();
        }
    }

    // Level 28: AMDHWChannel::allocateMemoryResources configures each channel's profiler with the CG reference clock
    // (hardware info +0xD8 x 10000, in Hz), 0 with the kernel's clocks left at Apple's zeros, and
    // AMDChannelProfiler::flushReportedData divides by it: with the stamps polled (28o) it ran and took a divide
    // error. The profiler alone gets the BC-250's 100 MHz when it is given 0.
    mach_vm_address_t orgProfilerConfigure = 0;
    UInt32            profilerConfigureLogged = 0;

    void wrapProfilerConfigure(void* profiler, bool enable, UInt64 frequency, UInt64 arg)
    {
        const auto given = frequency;
        if (frequency == 0) { frequency = kBc250RefClk * 10000; }
        if (++profilerConfigureLogged <= 8) {
            BCLOG("BC250HWL", "ChannelProfiler::configure(%d, %llu Hz -> %llu Hz, 0x%llX)", enable, given, frequency, arg);
        }
        FunctionCast(wrapProfilerConfigure, orgProfilerConfigure)(profiler, enable, frequency, arg);
    }

    // 28z5: compute stays off the MEC. GFX1013's compute engines are broken (Mesa and Linux run the BC-250 with no
    // compute queues: dispatches mis-execute, queue teardown wedges); Firefox's first Metal compute work on a MEC
    // pipe (HIQ-mapped queue, EOP ring 7, VMID 13) froze the machine within 0.2 s. Every accelerator channel takes
    // its hardware channel from AMDGFX10Hardware::getHWChannel(channel type, priority, index) (hardware vtable
    // +0x320; channel groups are GFX, compute, 4 x SDMA = types 0, 1, 2): a compute request (type 1) gets the GFX
    // channel, so Metal's compute command buffers run on the GFX ring, which executes dispatches correctly.
    // bc250compute=1 keeps Apple's MEC channels.
    mach_vm_address_t orgGetHwChannel = 0;
    UInt32            computeRemapped = 0;

    void* wrapGetHwChannel(void* hw, UInt32 type, UInt32 priority, UInt32 index)
    {
        auto org = FunctionCast(wrapGetHwChannel, orgGetHwChannel);
        if (type != 1) { return org(hw, type, priority, index); }
        void* gfx = org(hw, 0, priority, 0);
        if (gfx == nullptr) { gfx = org(hw, 0, 0, 0); }
        if (++computeRemapped <= 8) {
            BCLOG("BC250HWL", "compute channel (priority %u, index %u) -> GFX hardware channel %p (%s)", priority, index,
                gfx, className(gfx));
        }
        return gfx != nullptr ? gfx : org(hw, type, priority, index);
    }

    void hookHwInfo(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        UInt32 compute = 0;
        PE_parse_boot_argn("bc250compute", &compute, sizeof(compute));
        if (compute == 0) {
            KernelPatcher::RouteRequest channel {
                "__ZN31AMDRadeonX6000_AMDGFX10Hardware12getHWChannelE18_eAMD_CHANNEL_TYPE11SS_PRIORITYj",
                wrapGetHwChannel, orgGetHwChannel};
            if (patcher.routeMultiple(id, &channel, 1, slide, size) && orgGetHwChannel != 0) {
                BCLOG("BC250HWL", "level 28: compute channels run on the GFX ring (bc250compute=1 keeps the MEC)");
            }
            else {
                BCLOG("BC250HWL", "level 28: getHWChannel NOT hooked; compute stays on the MEC");
                patcher.clearError();
            }
        }
        KernelPatcher::RouteRequest profiler {"__ZN33AMDRadeonX6000_AMDChannelProfiler9configureEbyy",
            wrapProfilerConfigure, orgProfilerConfigure};
        if (!patcher.routeMultiple(id, &profiler, 1, slide, size)) {
            BCLOG("BC250HWL", "level 28: ChannelProfiler::configure hook not installed");
            patcher.clearError();
        }
        KernelPatcher::RouteRequest request {
            "__ZN29AMDRadeonX6000_AMDAccelDevice15getHardwareInfoEP24_sAMD_GET_HW_INFO_VALUES", wrapAccelHwInfo,
            orgAccelHwInfo};
        if (!patcher.routeMultiple(id, &request, 1, slide, size)) {
            BCLOG("BC250HWL", "level 28: getHardwareInfo hook not installed");
            patcher.clearError();
        }
    }

    void hookVmEntries(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        UInt32 pte = 1;
        PE_parse_boot_argn("bc250pte", &pte, sizeof(pte));
        if (pte == 0) {
            BCLOG("BC250HWL", "level 28: page-table entries as Apple's (bc250pte=0)");
            return;
        }
        KernelPatcher::RouteRequest requests[] = {
            {"__ZN26AMDRadeonX6000_AMDGFX10VMM11getPDEValueE15eAMD_VMPT_LEVELy", wrapGetPdeValue, orgGetPdeValue},
            {"__ZN26AMDRadeonX6000_AMDGFX10VMM11getPTEValueE15eAMD_VMPT_LEVELyN24AMDRadeonX6000_IAMDHWVMM10VmMapFlagsEj",
                wrapGetPteValue, orgGetPteValue},
            {"__ZN26AMDRadeonX6000_AMDGFX10VMM9decodePTEEyPyPb", wrapDecodePte, orgDecodePte},
            {"__ZN26AMDRadeonX6000_AMDGFX10VMM26prepareVMInvalidateRequestEP25AMD_VM_INVALIDATE_REQUESTPK22AMD_VM_"
             "INVALIDATE_INFOb",
                wrapPrepareVmInvalidate, orgPrepareVmInvalidate},
            {"__ZN29AMDRadeonX6000_AMDHWVMContext36updateContiguousPTEsWithDMAUsingAddrEyyyyy", wrapUpdatePtesDma,
                orgUpdatePtesDma},
            {"__ZN35AMDRadeonX6000_AMDGFX10HIQHWChannel20fillMapProcessPacketEP19PM4_MES_MAP_PROCESSyyjjj",
                wrapFillMapProcess, orgFillMapProcess},
        };
        UInt32 ptCpu = 1;
        PE_parse_boot_argn("bc250ptcpu", &ptCpu, sizeof(ptCpu));
        if (ptCpu != 0) {
            KernelPatcher::RouteRequest cpu[] = {
                {"__ZN29AMDRadeonX6000_AMDHWVMContext26prepareVmBlockForCPUAccessEyyPP11IOMemoryMap",
                    wrapPrepareCpuAccess, orgPrepareCpuAccess},
                {"__ZN29AMDRadeonX6000_AMDHWVMContext27completeVmBlockForCPUAccessEyPP11IOMemoryMap",
                    wrapCompleteCpuAccess, orgCompleteCpuAccess},
            };
            if (patcher.routeMultiple(id, cpu, arrsize(cpu), slide, size) && orgPrepareCpuAccess != 0 &&
                orgCompleteCpuAccess != 0)
            {
                BCLOG("BC250HWL", "level 28: page-table blocks past the BAR written through the carve-out");
            }
            else {
                BCLOG("BC250HWL", "level 28: VM block CPU access hooks NOT installed");
                patcher.clearError();
            }
        }
        UInt32 fullVramArg = 1;
        PE_parse_boot_argn("bc250fullvram", &fullVramArg, sizeof(fullVramArg));
        if (fullVramArg != 0) {
            KernelPatcher::RouteRequest space[] = {
                {"__ZN26AMDRadeonX6000_AMDHWMemory15setVirtualSpaceEP18IOMemoryDescriptor", wrapSetVirtualSpace,
                    orgSetVirtualSpace},
            };
            if (patcher.routeMultiple(id, space, arrsize(space), slide, size) && orgSetVirtualSpace != 0) {
                fullVram = true;
                BCLOG("BC250HWL", "level 28: full VRAM CPU-visible through the carve-out (bc250fullvram=0: off)");
            }
            else {
                BCLOG("BC250HWL", "level 28: setVirtualSpace hook NOT installed; VRAM visibility unchanged");
                patcher.clearError();
            }
        }
        if (patcher.routeMultiple(id, requests, arrsize(requests), slide, size)) {
            BCLOG("BC250HWL", "level 28: page-table entries and VM programs inside VRAM get the FB offset");
        }
        else {
            BCLOG("BC250HWL", "level 28: page-table entry hooks not installed");
            patcher.clearError();
        }
    }

    // SMU mailbox firewall (level 9 on). The TTL SMU SWIP writes every register through smu_cgs_write_register(smu,
    // reg, instance, value, size, flags): reg is MP1-relative unless instance is 0xFF, and its message send is
    // "C2PMSG_90 = 0; C2PMSG_66 = msg; poll C2PMSG_90" as in Linux's smu_cmn. Writes to MP1's C2PMSG registers
    // (MP1-relative 0x240-0x2BF, absolute 0x16240-0x162BF) are refused and logged, so no SMU message is sent until the
    // BC-250's SMU 11.0.8 message set is handled.
    const UInt8 kSmuCgsWritePattern[] = {0x55, 0x48, 0x89, 0xE5, 0x4C, 0x8B, 0x57, 0x08, 0x80, 0xBF, 0xD0, 0x02, 0x00,
        0x00, 0x00, 0x74, 0x0D, 0x44, 0x89, 0xC8, 0x09, 0xF0, 0x3B, 0x87, 0xCC, 0x02, 0x00, 0x00};
    constexpr UInt32 kMp1C2PMsgFirst = 0x240, kMp1C2PMsgLast = 0x2BF, kMp1Base = 0x16000;

    mach_vm_address_t orgSmuCgsWrite = 0;
    UInt32            smuWritesRefused = 0;

    UInt64 wrapSmuCgsWrite(void* smu, UInt32 reg, UInt32 instance, UInt32 value, UInt32 size, UInt32 flags)
    {
        const auto relative = instance == 0xFF ? reg - kMp1Base : reg;
        if (relative >= kMp1C2PMsgFirst && relative <= kMp1C2PMsgLast) {
            if (++smuWritesRefused <= 64) {
                BCLOG("BC250HWL", "SMU firewall: refused write of 0x%X to MP1 C2PMSG reg 0x%X (instance 0x%X)", value,
                    reg, instance);
            }
            return 0;
        }
        return FunctionCast(wrapSmuCgsWrite, orgSmuCgsWrite)(smu, reg, instance, value, size, flags);
    }

    UInt32 wrapVramRestore(void* config)
    {
        BCLOG("BC250HWL", "BGM teardown: 64 KB write-back to VRAM offset 0 skipped (%p)", config);
        return 0;
    }

    void hookTlsSwInit(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        const auto validate =
            findUnique(kIpiValidateTopologyPattern, nullptr, sizeof(kIpiValidateTopologyPattern), slide, size);
        const auto init =
            findUnique(kIpiInitIpInterfacesPattern, nullptr, sizeof(kIpiInitIpInterfacesPattern), slide, size);
        const auto restore = findUnique(kVramRestorePattern, nullptr, sizeof(kVramRestorePattern), slide, size);
        // Blocking IpiInitializeIpInterfaces without the write-back guard would let the teardown touch the
        // framebuffer; install all three or none.
        if (validate == 0 || init == 0 || restore == 0) {
            BCLOG("BC250HWL", "level 8: IpiValidateTopology %s, IpiInitializeIpInterfaces %s, VRAM restore %s; not hooked",
                validate ? "found" : "missing", init ? "found" : "missing", restore ? "found" : "missing");
            return;
        }
        // From level 9 the stop moves to TlsExecuteIpEntrySeq, and the SMU firewall must be in place as well.
        mach_vm_address_t execSeq = 0, smuWrite = 0;
        execSeq  = findUnique(kTlsExecuteIpEntrySeqPattern, nullptr, sizeof(kTlsExecuteIpEntrySeqPattern), slide,
             size);
        smuWrite = findUnique(kSmuCgsWritePattern, nullptr, sizeof(kSmuCgsWritePattern), slide, size);
        if (execSeq == 0 || smuWrite == 0) {
            BCLOG("BC250HWL", "level 9: TlsExecuteIpEntrySeq %s, smu_cgs_write_register %s; not hooked",
                execSeq ? "found" : "missing", smuWrite ? "found" : "missing");
            return;
        }
        // From level 10 the stop moves into the per-SWIP event dispatcher.
        mach_vm_address_t swipEvent = 0, providerLookup = 0;
        swipEvent = findUnique(kIpiSwipEventPattern, nullptr, sizeof(kIpiSwipEventPattern), slide, size);
        if (swipEvent == 0) {
            BCLOG("BC250HWL", "level 10: SWIP event dispatcher missing; not hooked");
            return;
        }
        // From level 12 every sw_init runs by default, and bc250hw selects how many SWIPs run hw_init.
        swipAllowed = arrsize(kSwipBootOrder);
        hwAllowed = hwLimit();
        // Logging only: a missing lookup does not stop level 10.
        providerLookup = findUnique(kSwipProviderLookupPattern, kSwipProviderLookupMask,
            sizeof(kSwipProviderLookupPattern), slide, size);
        if (providerLookup != 0) {
            swipProviderFn  = ripTarget(providerLookup + kProviderFnLoad);
            swipProviderCtx = ripTarget(providerLookup + kProviderCtxLoad);
        }
        else {
            BCLOG("BC250HWL", "level 10: SWIP provider lookup not found; not logged");
        }
        KernelPatcher::RouteRequest requests[] = {
            {nullptr, wrapIpiValidateTopology, orgIpiValidateTopology},
            {nullptr, wrapIpiInitIpInterfaces, orgIpiInitIpInterfaces},
            {nullptr, wrapVramRestore, orgVramRestore},
            {nullptr, wrapTlsExecuteIpEntrySeq, orgTlsExecuteIpEntrySeq},
            {nullptr, wrapSmuCgsWrite, orgSmuCgsWrite},
            {nullptr, wrapIpiSwipEvent, orgIpiSwipEvent},
        };
        requests[0].from = validate;
        requests[1].from = init;
        requests[2].from = restore;
        requests[3].from = execSeq;
        requests[4].from = smuWrite;
        requests[5].from = swipEvent;
        const size_t count = 6;
        if (patcher.routeMultiple(id, requests, count)) {
            BCLOG("BC250HWL", "level 9: TlsExecuteIpEntrySeq +0x%llX, smu_cgs_write_register +0x%llX hooked",
                execSeq - slide, smuWrite - slide);
            BCLOG("BC250HWL", "level 10: SWIP event dispatcher +0x%llX hooked, first %u SWIP(s) of the boot order "
                              "may run sw_init",
                swipEvent - slide, swipAllowed);
            hookGvmStages(patcher, id, slide, size);
            hookPspClassify(patcher, id, slide, size);
            BCLOG("BC250HWL", "level 12: hw_init for the first %u SWIP(s) of the boot order, DMCU stubbed",
                hwAllowed);
            setCailSettings();
            const auto reader =
                findUnique(kCailReadSettingPattern, nullptr, sizeof(kCailReadSettingPattern), slide, size);
            if (reader != 0) {
                KernelPatcher::RouteRequest request {nullptr, wrapCailReadSetting, orgCailReadSetting};
                request.from = reader;
                if (!patcher.routeMultiple(id, &request, 1)) {
                    BCLOG("BC250HWL", "level 12: CAIL setting reader failed to route");
                    patcher.clearError();
                }
            }
            else {
                BCLOG("BC250HWL", "level 12: CAIL setting reader not found; reads not logged");
            }
            const auto extTag =
                findUnique(kPcieSetExtTagPattern, nullptr, sizeof(kPcieSetExtTagPattern), slide, size);
            KernelPatcher::RouteRequest extTagRequest {nullptr, wrapPcieSetExtTag, orgPcieSetExtTag};
            extTagRequest.from = extTag;
            if (extTag == 0 || !patcher.routeMultiple(id, &extTagRequest, 1)) {
                // Without this guard BGM's hw_init could write PCI config 0x60 of unknown devices: keep hw_init
                // refused for every SWIP.
                BCLOG("BC250HWL", "level 12: PCIE extended-tag guard not installed; hw_init stays refused");
                patcher.clearError();
                hwAllowed = 0;
            }
            {
                if (hookGvmDryRun(patcher, id, slide, size)) {
                    BCLOG("BC250HWL", "level 13: GVM dry run hooked: its register writes and GPU memory copies are "
                                      "logged, not made");
                    enableGvmLive();
                }
                else if (hwAllowed >= kGvmHwPosition) {
                    BCLOG("BC250HWL", "level 13: GVM dry run not installed; GVM hw_init refused");
                    hwAllowed = kGvmHwPosition - 1;
                }
            }
            {
                if (hookPspDryRun(patcher, id, slide, size)) {
                    BCLOG("BC250HWL", "level 15: PSP dry run hooked: its register writes, ring create and commands "
                                      "are logged, not made");
                    {
                        pspLive = gvmLive;
                        fwLive  = pspLive;
                        if (fwLive) {
                            const PenguinWizardry::MaskedLookupPatch tmrAlign {&kextRadeonX6000HWLibs,
                                kPspTmrAlignOriginal, kPspTmrAlignPatched, sizeof(kPspTmrAlignOriginal), 1};
                            const bool aligned = tmrAlign.apply(patcher, slide, size);
                            if (!aligned) { patcher.clearError(); }
                            BCLOG("BC250HWL", "level 17: TMR alignment %s", aligned ? "4 MB" : "patch failed, 1 MB");
                        }
                        BCLOG("BC250HWL", "level 16: PSP ring and TMR %s", pspLive ? "made (ASD, TAs, TOC and "
                            "firmware loads still logged, not sent)" : "stay a dry run (GVM is not live)");
                        if (fwLive) {
                            BCLOG("BC250HWL", "level 17: PSP loads the BC-250's cyan_skillfish2 microcode (ME, PFP, "
                                              "CE, MEC, MEC2, RLC_G, SDMA0/1)");
                        }
                    }
                }
                else if (hwAllowed >= kPspHwPosition) {
                    BCLOG("BC250HWL", "level 15: PSP dry run not installed; PSP hw_init refused");
                    hwAllowed = kPspHwPosition - 1;
                }
            }
            {
                hookGcLog(patcher, id, slide, size);
                if (hookGcDryRun(patcher, id, slide, size)) {
                    BCLOG("BC250HWL", "level 18: GC dry run hooked: its register writes are logged, not made");
                    {
                        gcLive = fwLive;
                        BCLOG("BC250HWL", "level 19: GC's writes %s", gcLive ? "are made (SPM sample delays dropped)"
                                                                       : "stay a dry run (firmware not loaded live)");
                    }
                }
                else if (hwAllowed >= kGcHwPosition) {
                    BCLOG("BC250HWL", "level 18: GC dry run not installed; GC hw_init refused");
                    hwAllowed = kGcHwPosition - 1;
                }
            }
            {
                const auto at = findUnique(kIpiQueryPattern, nullptr, sizeof(kIpiQueryPattern), slide, size);
                KernelPatcher::RouteRequest request {nullptr, wrapIpiQuery, orgIpiQuery};
                request.from = at;
                const auto vcn = findFunction(patcher, id, "_IpiGetVcnContext", kIpiVcnContextPattern,
                    sizeof(kIpiVcnContextPattern), slide, size);
                KernelPatcher::RouteRequest vcnRequest {nullptr, wrapIpiVcnContext};
                vcnRequest.from = vcn;
                const bool vcnOk = vcn != 0 && patcher.routeMultipleShort(id, &vcnRequest, 1);
                if (!vcnOk) { patcher.clearError(); }
                if (vcnOk && at != 0 && patcher.routeMultiple(id, &request, 1)) {
                    BCLOG("BC250HWL", "level 22: IPI query router and VCN context hooked (stubbed VCN absent)");
                }
                else {
                    // Without it TTL's post-init calls into the empty VCN context: keep TTL short of completing.
                    patcher.clearError();
                    hwAllowed = kSdmaHwPosition;
                    BCLOG("BC250HWL", "level 22: IPI query router or VCN context not hooked; hw_init stops after "
                                      "SDMA");
                }
            }
            {
                if (hookSdmaDryRun(patcher, id, slide, size)) {
                    BCLOG("BC250HWL", "level 20: SDMA dry run hooked: its register writes are logged, not made");
                    {
                        sdmaLive = gcLive;
                        BCLOG("BC250HWL", "level 21: SDMA's writes %s", sdmaLive ? "are made (Linux's engine start "
                            "values)" : "stay a dry run (GC is not live)");
                    }
                }
                else if (hwAllowed >= kSdmaHwPosition) {
                    BCLOG("BC250HWL", "level 20: SDMA dry run not installed; SDMA hw_init refused");
                    hwAllowed = kSdmaHwPosition - 1;
                }
            }
            if (providerLookup != 0) {
                KernelPatcher::RouteRequest request {nullptr, wrapSwipProviderLookup, orgSwipProviderLookup};
                request.from = providerLookup;
                if (patcher.routeMultiple(id, &request, 1)) {
                    BCLOG("BC250HWL", "level 10: SWIP provider lookup +0x%llX hooked (provider globals at +0x%llX, "
                                      "+0x%llX)",
                        providerLookup - slide, reinterpret_cast<mach_vm_address_t>(swipProviderFn) - slide,
                        reinterpret_cast<mach_vm_address_t>(swipProviderCtx) - slide);
                }
                else {
                    BCLOG("BC250HWL", "level 10: SWIP provider lookup failed to route");
                    patcher.clearError();
                }
            }
            tlsStopInstalled = true;
            BCLOG("BC250HWL", "level 8: IpiValidateTopology +0x%llX, IpiInitializeIpInterfaces +0x%llX, VRAM restore "
                              "+0x%llX hooked",
                validate - slide, init - slide, restore - slide);
        }
        else {
            BCLOG("BC250HWL", "level 8: failed to route");
            patcher.clearError();
        }
    }

    void hookBgmCreate(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        const auto bgm = findUnique(kBgmCreatePattern, nullptr, sizeof(kBgmCreatePattern), slide, size);
        size_t     unused = 0;
        if (bgm == 0 || !KernelPatcher::findPattern(kBgmCreateError203, nullptr, sizeof(kBgmCreateError203),
                            reinterpret_cast<const void*>(bgm), kBgmCreateSpan, &unused))
        {
            BCLOG("BC250HWL", "bgm_create not found; version remap not installed");
            return;
        }
        const auto call = findUnique(kParseCallPattern, kParseCallPatternMask, sizeof(kParseCallPattern), bgm,
            kBgmCreateSpan);
        if (call == 0) {
            BCLOG("BC250HWL", "bgm_create's discovery parse call not found; version remap not installed");
            return;
        }
        const auto parse = call + kParseCallRel + 4 + getMember<SInt32>(reinterpret_cast<void*>(call), kParseCallRel);
        if (parse < slide || parse >= slide + size) {
            BCLOG("BC250HWL", "discovery parse target out of range; version remap not installed");
            return;
        }
        KernelPatcher::RouteRequest requests[] = {
            {nullptr, wrapBgmCreate, orgBgmCreate},
            {nullptr, wrapDiscoveryParse, orgDiscoveryParse},
        };
        requests[0].from = bgm;
        requests[1].from = parse;
        if (patcher.routeMultiple(id, requests, arrsize(requests))) {
            BCLOG("BC250HWL", "bgm_create at +0x%llX and discovery parse at +0x%llX hooked", bgm - slide,
                parse - slide);
        }
        else {
            BCLOG("BC250HWL", "bgm_create/discovery parse failed to route");
            patcher.clearError();
        }
    }

}    // namespace

// Freed VRAM goes to IOGraphicsAccelerator2's orphaned-memory pool at +0xBC8 (IOAccelVidMemory::orphanIt ->
// addMemory). A buffer flagged reusable at creation (IOAccelVidMemory::init: setFlags(1) when bit 28 of the config
// word +0xC90 is set) lands in a size-hashed list that only an exact-size allocation reuses (allocMemory) and that no
// collector trims (the reusable pruner runs on the +0xB60 pool only; +0xBC8 has no size limit); IOAccelResource2::
// allocMemory then fails without releasing it. Opening many apps at 4K left 1.3-2.4 GB there with 7-80 MB of VRAM
// free and thousands of "Failed to allocate". A non-reusable buffer goes to the other list and kicks the collector,
// which frees it. Bit 28 is cleared (bc250vramreuse=1 keeps it) so freed VRAM is returned at once: no reuse cache;
// buffers created before the accelerator is found keep their flag. Tahoe 26.7.1 layout.
void BC250HWL::accelPoolTick()
{
    static IOService* accel = nullptr;
    if (accel == nullptr) {
        auto* match = IOService::serviceMatching("IOGraphicsAccelerator2");
        if (match == nullptr) { return; }
        accel = IOService::copyMatchingService(match);
        match->release();
        if (accel == nullptr) { return; }
        UInt32 reuse = 0;
        PE_parse_boot_argn("bc250vramreuse", &reuse, sizeof(reuse));
        auto&        config = getMember<UInt32>(accel, 0xC90);
        const UInt32 was    = config;
        if (reuse == 0) { config = was & ~0x10000000U; }
        BCLOG("BC250HWL", "accelerator %s: config 0x%08X -> 0x%08X (VRAM reuse %s)", accel->getMetaClass()->getClassName(),
            was, config, (config & 0x10000000U) != 0 ? "on" : "off");
    }
}

// 28x: SDMA1 never raises its trap (no client 9 source 0xE0 IV): its SDMA_CNTL TRAP_ENABLE (bit 0) stays clear while
// SDMA0's is set once the accelerator enables its SDMA0 trap interrupt (IRQMgr), so SDMA1's fences complete unseen
// (channel 14 timeouts, the display pipe's transactions waiting on them). Mirrored from SDMA0 every 0.2 s
// (bc250sdma1trap=0 disables).
void BC250HWL::sdma1TrapPoll()
{
    static SInt32 mirror = -1;
    if (mirror < 0) {
        UInt32 trap = 1;
        PE_parse_boot_argn("bc250sdma1trap", &trap, sizeof(trap));
        mirror = trap != 0;
    }
    // Only once the AMD kexts load: before that SDMA may be powered down, and reading the registers of a powered-down
    // block can hang the bus.
    auto& nred = NRed::singleton();
    if (!mirror || !amdKextsLoaded || nred.getMMIOLength() == 0) { return; }
    constexpr UInt32 kSdma0Cntl = 0x127C, kSdma1Cntl = 0x187C;
    const UInt32     s0 = nred.readReg32(kSdma0Cntl), s1 = nred.readReg32(kSdma1Cntl);
    if ((s0 & 1) != 0 && (s1 & 1) == 0 && s1 != 0xFFFFFFFF) { nred.writeReg32(kSdma1Cntl, s1 | 1); }
}

// HWLibs knows the BC-250 neither in its firmware device type table ("Unable to find matching firmware device type
// for device ID: 0x13FE") nor in its CAIL_ASIC_CAPS_TABLE. Both are stripped; they are found by content and the
// Navi 10 (0x731F) entries are turned into BC-250 ones. Navi 10 is a stand-in: the right firmware is
// cyan_skillfish2's, which HWLibs does not have (it is only loaded once PowerPlay/the accelerator start).
static bool looksLikeDeviceTypeEntry(const UInt32* e)
{
    return ((e[0] >= 0x6800 && e[0] <= 0x69FF) || (e[0] >= 0x7300 && e[0] <= 0x74FF)) &&
           e[1] < kAMDDeviceTypeUnknown;
}

static void patchHWLibsTables(mach_vm_address_t slide, size_t size)
{
    auto&      nred  = NRed::singleton();
    auto*      words = reinterpret_cast<UInt32*>(slide);
    const auto count = size / sizeof(UInt32);

    UInt32* typeEntry = nullptr;
    for (size_t i = 2; i + 3 < count; i++) {
        auto* e = words + i;
        // On 14.8.9 the table (in __TEXT) runs {0x731B,3} {0x731F,3} {0x4E,3}: the next ID is a small internal
        // one, so only the previous entry is checked by ID and the next one by its type.
        if (e[0] == 0x731F && e[1] == kAMDDeviceTypeNavi10 && looksLikeDeviceTypeEntry(e - 2) &&
            e[3] < kAMDDeviceTypeUnknown) {
            typeEntry = e;
            break;
        }
    }

    CAILAsicCapsEntry* capsEntry = nullptr;
    for (size_t i = 0; i + sizeof(CAILAsicCapsEntry) / sizeof(UInt32) < count; i += 2) {
        auto* e = reinterpret_cast<CAILAsicCapsEntry*>(words + i);
        if (e->familyId == AMD_FAMILY_NAVI && e->deviceId == 0x731F &&
            reinterpret_cast<UInt64>(e->ddiCaps) >= 0xFFFFFF7F00000000ULL &&
            reinterpret_cast<UInt64>(e->skeleton) >= 0xFFFFFF7F00000000ULL) {
            capsEntry = e;
            break;
        }
    }

    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
        BCLOG("BC250HWL", "cannot enable kernel writing; HWLibs tables left alone");
        return;
    }
    if (typeEntry != nullptr) {
        typeEntry[0] = nred.getDeviceID();
        BCLOG("BC250HWL", "deviceTypeTable: 0x731F entry now 0x%X (Navi 10 firmware type)", typeEntry[0]);
    }
    else {
        BCLOG("BC250HWL", "deviceTypeTable: no Navi 10 entry found");
    }
    if (capsEntry != nullptr) {
        capsEntry->deviceId    = nred.getDeviceID();
        capsEntry->revision    = nred.getDevRevision();
        capsEntry->extRevision = static_cast<UInt32>(nred.getEnumRevision()) + nred.getDevRevision();
        capsEntry->pciRevision = nred.getPciRevision();
        BCLOG("BC250HWL", "CAIL_ASIC_CAPS_TABLE: Navi 10 row now device 0x%X rev 0x%X ext 0x%X pci 0x%X",
            capsEntry->deviceId, capsEntry->revision, capsEntry->extRevision, capsEntry->pciRevision);
    }
    else {
        BCLOG("BC250HWL", "CAIL_ASIC_CAPS_TABLE: no Navi 10 row found");
    }
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

// TTL's supported-ASIC table (80-byte rows {family, ext rev, device ID, rev, PCI rev, descriptor, ...}; ttl_initialize
// looks up device ID and PCI revision and fails with "unsupported" otherwise). Navi 10 is listed only with PCI revisions
// 0xC0-0xEB; the first revision-2 row (0x731F / 0xC0, rev 2 like the caps row above) becomes the BC-250's.
static void patchTtlAsicTable(mach_vm_address_t slide, size_t size)
{
    auto&      nred  = NRed::singleton();
    auto*      words = reinterpret_cast<UInt64*>(slide);
    const auto count = size / sizeof(UInt64);

    UInt64* row = nullptr;
    for (size_t i = 10; i + 10 <= count; i++) {
        auto* r = words + i;
        if (r[0] == AMD_FAMILY_NAVI && r[1] == 3 && r[2] == 0x731F && r[3] == 2 && r[4] == 0xC0 &&
            r[5] >= 0xFFFFFF7F00000000ULL && r[-10] == AMD_FAMILY_NAVI)
        {
            row = r;
            break;
        }
    }
    if (row == nullptr) {
        BCLOG("BC250HWL", "TTL ASIC table: no Navi 10 rev 2 row found");
        return;
    }
    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
        BCLOG("BC250HWL", "TTL ASIC table: cannot enable kernel writing");
        return;
    }
    row[2] = nred.getDeviceID();
    row[4] = nred.getPciRevision();
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    BCLOG("BC250HWL", "TTL ASIC table: Navi 10 row at +0x%llX now device 0x%llX PCI rev 0x%llX",
        reinterpret_cast<mach_vm_address_t>(row) - slide, row[2], row[4]);
}

void BC250HWL::processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
{
    if (!NRed::singleton().getAttributes().isCyanSkillfish() || !BC250::singleton().isHWLSurvey()) { return; }

    Kext kext;
    if (kextRadeonX6000Framebuffer.loadIndex == id) {
        kext = Kext::FB;
    }
    else if (kextRadeonX6000HWServices.loadIndex == id) {
        kext = Kext::HWServices;
    }
    else if (kextRadeonX6000HWLibs.loadIndex == id) {
        kext = Kext::HWLibs;
    }
    else if (kextRadeonX6000.loadIndex == id) {
        kext = Kext::Accel;
    }
    else {
        return;
    }

    amdKextsLoaded = true;
    kextRanges[static_cast<UInt8>(kext)].start = slide;
    kextRanges[static_cast<UInt8>(kext)].end   = slide + size;
    static const char* const names[] = {"AMDRadeonX6000Framebuffer", "AMDRadeonX6000HWServices",
        "AMDRadeonX6000HWLibs", "AMDRadeonX6000"};
    BCLOG("BC250HWL", "%s loaded", names[static_cast<UInt8>(kext)]);

    if (kext == Kext::HWLibs) {
        hwlibsStart = slide;
        hwlibsEnd   = slide + size;
        patchHWLibsTables(slide, size);
        patchTtlAsicTable(slide, size);
        patchDiscoveryVersion(slide, size);
        hookTlsSwInit(patcher, id, slide, size);
        hookBgmCreate(patcher, id, slide, size);
    }
    if (kext == Kext::Accel) { guardUnmapDoorbellMemory(patcher, id, slide, size); }
    if (kext == Kext::Accel) {
        accelLive = true;
        // Level 27: the KIQ's first packet, SET_RESOURCES, with Linux's first dword (gfx_v10_0_kiq_set_resources:
        // VMID_MASK 0, unmap latency 0) instead of Apple's (all 16 VMIDs to the firmware scheduler, latency 0x28);
        // with Apple's the MEC stalls decoding it (level 27 runs). bc250kiq=0 keeps Apple's.
        UInt32 kiq = 1;
        PE_parse_boot_argn("bc250kiq", &kiq, sizeof(kiq));
        if (kiq != 0) {
            const PenguinWizardry::MaskedLookupPatch setResources {&kextRadeonX6000, kKiqSetResourcesOriginal,
                kKiqSetResourcesPatched, sizeof(kKiqSetResourcesOriginal), 1};
            const bool ok = setResources.apply(patcher, slide, size);
            if (!ok) { patcher.clearError(); }
            BCLOG("BC250HWL", "level 27: KIQ SET_RESOURCES %s", ok ? "as Linux (VMID mask 0)" : "patch failed, Apple's");
        }
    }
    if (kext == Kext::Accel) { hookAccelRegisters(patcher, id, slide, size); }
    if (kext == Kext::Accel) {
        hookVmEntries(patcher, id, slide, size);
        hookHwInfo(patcher, id, slide, size);
        hookHubInvalidateRequests(patcher, id, slide, size);
    }
    if (kext == Kext::FB) {
        KernelPatcher::RouteRequest start {"__ZN34AMDRadeonX6000_AmdInterruptManager15startInterruptsEP10IOWorkLoop",
            wrapStartInterrupts, orgStartInterrupts};
        if (!patcher.routeMultiple(id, &start, 1, slide, size) || orgStartInterrupts == 0) {
            BCLOG("BC250HWL", "level 28: AmdInterruptManager::startInterrupts NOT hooked; no SDMA1 notify timer");
            patcher.clearError();
        }
    }
    if (kext == Kext::FB) {
        KernelPatcher::RouteRequest request {
            "__ZN34AMDRadeonX6000_AmdRadeonController28callPlatformFunctionFromDrvrEjPvS0_S0_",
            wrapControllerDrvrFunction, orgControllerDrvrFunction};
        if (!patcher.routeMultiple(id, &request, 1, slide, size)) {
            BCLOG("BC250HWL", "level 25: controller request guard not installed; Hardware::init stops as at level 24");
            patcher.clearError();
            accelStopUnguarded = true;
        }
    }
    if (kext == Kext::Accel) {
        UInt32 stats = 1;
        PE_parse_boot_argn("bc250stats", &stats, sizeof(stats));
        if (stats != 0) {
            KernelPatcher::RouteRequest request {"__ZN26AMDRadeonX6000_AMDHardware19publishPMStatisticsEP12OSDictionaryb",
                wrapPublishPMStatistics};
            if (patcher.routeMultiple(id, &request, 1, slide, size)) {
                BCLOG("BC250HWL", "level 28: GPU statistics (clock, temperature, activity) from the SMU telemetry");
            }
            else {
                BCLOG("BC250HWL", "level 28: publishPMStatistics not replaced; GPU statistics stay Apple's");
                patcher.clearError();
            }
        }
    }
    if (kext == Kext::Accel) {
        KernelPatcher::RouteRequest stopRequest {"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator4stopEP9IOService",
            wrapAccelStop, orgAccelStop};
        if (!patcher.routeMultiple(id, &stopRequest, 1, slide, size)) {
            // Without the guard a late start failure panics: keep Hardware::init short of succeeding (level 24).
            BCLOG("BC250HWL", "level 25: accelerator stop guard not installed; Hardware::init stops as at level 24");
            patcher.clearError();
            accelStopUnguarded = true;
        }
    }

    for (size_t i = 0; i < arrsize(hooks); i++) {
        auto& hook = hooks[i];
        if (hook.kext != kext) { continue; }
        KernelPatcher::RouteRequest request {hook.symbol, wrapperFor(i, MakeSeq<arrsize(hooks)>::Type {}),
            hook.org};
        if (!patcher.routeMultiple(id, &request, 1, slide, size) || hook.org == 0) {
            BCLOG("BC250HWL", "%s not hooked", hook.name);
            patcher.clearError();
        }
    }
}
