// ASRock BC-250 (AMD Cyan Skillfish) display engine quirks on top of AMDRadeonX6000Framebuffer's DCN 2.0 code
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include <BC250.hpp>
#include <BC250DCN.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <NRed.hpp>
#include <Regs/CyanSkillfish.hpp>
#include <kern/thread.h>
#include <sys/sysctl.h>

static BC250DCN moduleInstance;

BC250DCN& BC250DCN::singleton() { return moduleInstance; }

namespace
{
    // DCN 2.0.1 registers (dcn_2_0_1_offset.h on the DMU segments 0x12/0xC0/0x34C0/0x9000).
    constexpr UInt32 DENTIST_DISPCLK_CNTL                = 0xC0 + 0x64;
    constexpr UInt32 DENTIST_DISPCLK_WDIVIDER_MASK       = 0x7F;
    constexpr UInt32 DENTIST_DPPCLK_WDIVIDER_SHIFT       = 24;
    constexpr UInt32 DENTIST_DISPCLK_CHG_DONE            = 1U << 19;
    constexpr UInt32 DENTIST_DPPCLK_CHG_DONE             = 1U << 20;
    constexpr UInt32 DENTIST_DID_MIN                     = 0x0A;    // Divider 2.5, what the GOP runs at.
    constexpr UInt32 DENTIST_DID_MAX                     = 0x7E;    // 0x7F is special (bypass); left alone.
    // CLK4 (clk_11_0_1_offset.h, CLK base 0x16C00): the DENTIST VCO and DPREFCLK, as dcn201_clk_mgr reads them.
    constexpr UInt32 CLK4_CLK_PLL_REQ                    = 0x16C00 + 0x460E;
    constexpr UInt32 CLK4_CLK2_CURRENT_CNT               = 0x16C00 + 0x467F;

    constexpr size_t PLANE_SCAN_BYTES  = 0x200;
    constexpr size_t CURSOR_SCAN_BYTES = 0x80;
    constexpr size_t MAX_SAVED         = 8;

    const char kApplyPipeSplitFlags[] =
        "int dcn20_validate_apply_pipe_split_flags(struct dc *, struct dc_state *, int, int *, _Bool *)";
    constexpr size_t MAX_PIPES = 6;    // Navi 10's DAL, the size of the split[] arrays it passes around.

    const char kFindSecondaryPipe[] =
        "struct pipe_ctx *dcn20_find_secondary_pipe(struct dc *, struct resource_context *, "
        "const struct resource_pool *, const struct pipe_ctx *)";

    // DENTIST divider IDs in quarter steps (dcn20_clk_mgr.c ranges 1 and 2; the 7-bit field never reaches 3).
    UInt32 didToDiv4(UInt32 did) { return did < 64 ? did : 64 + (did - 64) * 2; }
    UInt32 div4ToDid(UInt32 div4) { return div4 < 64 ? div4 : 64 + (div4 - 64) / 2; }

    // Finds the function that references `str` with a RIP-relative LEA, by walking back to its prologue. Every
    // reference has to resolve to the same function, otherwise nothing is returned.
    mach_vm_address_t findFunctionByString(mach_vm_address_t start, size_t size, const char* str, size_t len)
    {
        size_t strOff = 0;
        if (!KernelPatcher::findPattern(str, nullptr, len, reinterpret_cast<const void*>(start), size, &strOff)) {
            return 0;
        }
        const auto strAddr = start + strOff;
        const auto* p      = reinterpret_cast<const UInt8*>(start);

        mach_vm_address_t found = 0;
        for (size_t i = 0; i + 7 <= size; i++) {
            if ((p[i] != 0x48 && p[i] != 0x4C) || p[i + 1] != 0x8D || (p[i + 2] & 0xC7) != 0x05) { continue; }
            SInt32 disp;
            memcpy(&disp, p + i + 3, sizeof(disp));
            if (static_cast<mach_vm_address_t>(static_cast<SInt64>(start + i + 7) + disp) != strAddr) { continue; }
            mach_vm_address_t fn = 0;
            for (size_t j = i; j >= 4 && i - j < 0x8000; j--) {
                if (p[j] == 0x55 && p[j + 1] == 0x48 && p[j + 2] == 0x89 && p[j + 3] == 0xE5) {
                    const UInt8 prev = p[j - 1];
                    if ((j % 16) == 0 || prev == 0xC3 || prev == 0xCC || prev == 0x90 || prev == 0x00) {
                        fn = start + j;
                        break;
                    }
                }
            }
            if (fn == 0 || (found != 0 && found != fn)) { return 0; }
            found = fn;
        }
        return found;
    }
}    // namespace

SYSCTL_DECL(_debug_bc250);
SYSCTL_UINT(_debug_bc250, OID_AUTO, addrfixes, CTLFLAG_RD | CTLFLAG_LOCKED, &moduleInstance.addressFixes, 0,
            "scanout addresses translated to UMA");
SYSCTL_UINT(_debug_bc250, OID_AUTO, clockfixes, CTLFLAG_RD | CTLFLAG_LOCKED, &moduleInstance.clockFixes, 0,
            "DENTIST divider corrections");
SYSCTL_UINT(_debug_bc250, OID_AUTO, piperemaps, CTLFLAG_RD | CTLFLAG_LOCKED, &moduleInstance.pipeRemaps, 0,
            "MPC split pipes moved from 4/5 to 2/3");
SYSCTL_UINT(_debug_bc250, OID_AUTO, vcokhz, CTLFLAG_RD | CTLFLAG_LOCKED, &moduleInstance.vcoKHz, 0,
            "DENTIST VCO in kHz");

void BC250DCN::processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
{
    auto& nred = NRed::singleton();
    using namespace CyanSkillfish;
    this->fbBase   = static_cast<UInt64>(nred.readReg32(GCMC_VM_FB_LOCATION_BASE) & 0xFFFFFF) << 24;
    this->fbTop    = static_cast<UInt64>((nred.readReg32(GCMC_VM_FB_LOCATION_TOP) & 0xFFFFFF) + 1) << 24;
    this->fbOffset = static_cast<UInt64>(nred.readReg32(GCMC_VM_FB_OFFSET) & 0xFFFFFF) << 24;
    BCLOG("BC250DCN", "FB MC 0x%llX-0x%llX is system 0x%llX", this->fbBase, this->fbTop, this->fbOffset);

    sysctl_register_oid(&sysctl__debug_bc250_addrfixes);
    sysctl_register_oid(&sysctl__debug_bc250_clockfixes);
    sysctl_register_oid(&sysctl__debug_bc250_piperemaps);
    sysctl_register_oid(&sysctl__debug_bc250_vcokhz);

    // 1. Scanout addresses. Each hook is optional: a missing one is logged, not fatal.
    KernelPatcher::RouteRequest requests[] = {
        {"__ZN27AMDRadeonX6000_AmdDalHelper34prepareDalDisplaySurfaceParametersEPK16AmdFbDisplayPathP20AmdFalconUpdate"
         "Plane",
            wrapPrepareSurface, this->orgPrepareSurface},
        {"__ZN27AMDRadeonX6000_AmdDalHelper17updateSurfaceInfoEPK20AmdFalconUpdatePlanePK16AmdFbDisplayPath",
            wrapUpdateSurfaceInfo, this->orgUpdateSurfaceInfo},
        {"__ZN27AMDRadeonX6000_AmdDalHelper14setCursorImageEPK21AmdFbCursorDescriptor", wrapSetCursorImage,
            this->orgSetCursorImage},
        {"__ZN27AMDRadeonX6000_AmdDalHelper22dalGetRegistryPropertyEPvPKcS0_m", wrapGetRegistryProperty,
            this->orgGetRegistryProperty},
    };
    for (auto& request : requests) {
        if (!patcher.routeMultiple(id, &request, 1, slide, size)) {
            BCLOG("BC250DCN", "Failed to route %s", request.symbol);
            patcher.clearError();
        }
    }

    // 3. MPC splits. By default the DAL is kept from splitting at all (one pipe per stream, like the GOP), since
    // the partner pipe of a split does not come out right on DCN 2.0.1. -BC250PipeRemap instead moves the partner
    // from pipe 4/5 to 2/3 (experimental).
    if (checkKernelArgument("-BC250PipeRemap")) {
        const auto fn = findFunctionByString(slide, size, kFindSecondaryPipe, sizeof(kFindSecondaryPipe));
        KernelPatcher::RouteRequest request {nullptr, wrapFindSecondaryPipe, this->orgFindSecondaryPipe};
        request.from = fn;
        if (fn == 0 || !patcher.routeMultiple(id, &request, 1)) {
            BCLOG("BC250DCN", "dcn20_find_secondary_pipe not routed");
            patcher.clearError();
        }
    }
    else {
        const auto fn = findFunctionByString(slide, size, kApplyPipeSplitFlags, sizeof(kApplyPipeSplitFlags));
        KernelPatcher::RouteRequest request {nullptr, wrapApplyPipeSplitFlags, this->orgApplyPipeSplitFlags};
        request.from = fn;
        if (fn == 0 || !patcher.routeMultiple(id, &request, 1)) {
            BCLOG("BC250DCN", "dcn20_validate_apply_pipe_split_flags not routed; splits may use missing pipes");
            patcher.clearError();
        }
        else {
            BCLOG("BC250DCN", "dcn20_validate_apply_pipe_split_flags at 0x%llX routed", fn);
        }
    }

    // 2. DENTIST VCO, as dcn201_clk_mgr_construct reads it (FbMult in units of 100 MHz).
    const auto pllReq  = nred.readReg32(CLK4_CLK_PLL_REQ);
    const auto fbInt   = pllReq & 0x1FF;
    const auto fbFrac  = pllReq >> 16;
    this->vcoKHz       = fbInt * 100000 + static_cast<UInt32>((static_cast<UInt64>(fbFrac) * 100000) >> 16);
    UInt32 overrideKHz = 0;
    if (PE_parse_boot_argn("bc250vco", &overrideKHz, sizeof(overrideKHz)) && overrideKHz != 0) {
        this->vcoKHz = overrideKHz;
    }
    if (this->vcoKHz < 1000000 || this->vcoKHz > 6000000) {
        BCLOG("BC250DCN", "Implausible DENTIST VCO %u kHz (CLK4_CLK_PLL_REQ 0x%X); using 2670000", this->vcoKHz,
            pllReq);
        this->vcoKHz = 2670000;
    }
    PE_parse_boot_argn("bc250applevco", &this->appleVcoKHz, sizeof(this->appleVcoKHz));
    BCLOG("BC250DCN", "DENTIST VCO %u kHz (CLK4_CLK_PLL_REQ 0x%X), DPREFCLK %u kHz, DAL assumes %u kHz",
        this->vcoKHz, pllReq, nred.readReg32(CLK4_CLK2_CURRENT_CNT) * 100, this->appleVcoKHz);
    if (checkKernelArgument("-BC250NoClockFix")) {
        BCLOG("BC250DCN", "DENTIST watcher disabled by -BC250NoClockFix");
    }
    else {
        this->startClockWatcher();
    }
}

// -- 1. Scanout addresses (MC -> UMA) --

bool BC250DCN::toUMA(UInt64& addr) const
{
    if (addr < this->fbBase || addr >= this->fbTop) { return false; }
    addr = addr - this->fbBase + this->fbOffset;
    return true;
}

// Rewrites every 8-byte aligned MC carve-out address in [base, base + length). The range 0xF400000000+ is
// distinctive enough that nothing but an address lands in it. Optionally records originals for restoreWindow.
size_t BC250DCN::translateWindow(void* base, size_t length, const char* who, UInt64* saved, size_t* savedOff,
    size_t maxSaved)
{
    if (base == nullptr) { return 0; }
    size_t count = 0;
    auto*  words = static_cast<UInt64*>(base);
    for (size_t i = 0; i < length / sizeof(UInt64); i++) {
        UInt64 value = words[i];
        if (!this->toUMA(value)) { continue; }
        if (saved != nullptr) {
            if (count >= maxSaved) { break; }
            saved[count]    = words[i];
            savedOff[count] = i;
        }
        if (this->addressFixes < 8) {
            BCLOG("BC250DCN", "%s: +0x%zX 0x%llX -> 0x%llX", who, i * sizeof(UInt64), words[i], value);
        }
        words[i] = value;
        count += 1;
        this->addressFixes += 1;
    }
    return count;
}

void BC250DCN::restoreWindow(void* base, const UInt64* saved, const size_t* savedOff, size_t count)
{
    auto* words = static_cast<UInt64*>(base);
    for (size_t i = 0; i < count; i++) { words[savedOff[i]] = saved[i]; }
}

UInt64 BC250DCN::wrapPrepareSurface(void* self, const void* displayPath, void* plane)
{
    auto&      s   = singleton();
    const auto ret = FunctionCast(wrapPrepareSurface, s.orgPrepareSurface)(self, displayPath, plane);
    s.translateWindow(plane, PLANE_SCAN_BYTES, "prepareSurface", nullptr, nullptr, 0);
    return ret;
}

UInt64 BC250DCN::wrapUpdateSurfaceInfo(void* self, const void* plane, const void* displayPath)
{
    auto&  s = singleton();
    UInt64 saved[MAX_SAVED];
    size_t off[MAX_SAVED];
    auto*  mutablePlane = const_cast<void*>(plane);
    const auto n        = s.translateWindow(mutablePlane, PLANE_SCAN_BYTES, "updateSurfaceInfo", saved, off, MAX_SAVED);
    const auto ret      = FunctionCast(wrapUpdateSurfaceInfo, s.orgUpdateSurfaceInfo)(self, plane, displayPath);
    s.restoreWindow(mutablePlane, saved, off, n);
    return ret;
}

UInt64 BC250DCN::wrapSetCursorImage(void* self, const void* cursor)
{
    auto&  s = singleton();
    UInt64 saved[MAX_SAVED];
    size_t off[MAX_SAVED];
    auto*  mutableCursor = const_cast<void*>(cursor);
    const auto n = s.translateWindow(mutableCursor, CURSOR_SCAN_BYTES, "setCursorImage", saved, off, MAX_SAVED);
    const auto ret = FunctionCast(wrapSetCursorImage, s.orgSetCursorImage)(self, cursor);
    s.restoreWindow(mutableCursor, saved, off, n);
    return ret;
}

// Logs the DAL's option names once each, to find its pipe split/clock knobs. The key is only read if it looks
// like a C string, in case the callback's arguments are laid out differently than assumed.
UInt64 BC250DCN::wrapGetRegistryProperty(void* ctx, const char* key, void* value, size_t size)
{
    const auto ret = FunctionCast(wrapGetRegistryProperty, singleton().orgGetRegistryProperty)(ctx, key, value, size);
    static UInt32 logged = 0;
    if (logged < 96 && reinterpret_cast<UInt64>(key) >= 0xFFFFFF8000000000ULL) {
        bool printable = true;
        size_t len     = 0;
        for (; len < 64 && key[len] != '\0'; len++) {
            if (key[len] < 0x20 || key[len] > 0x7E) {
                printable = false;
                break;
            }
        }
        if (printable && len > 0 && len < 64) {
            UInt32 first = 0;
            if (value != nullptr && size >= sizeof(first)) { memcpy(&first, value, sizeof(first)); }
            BCLOG("BC250DCN", "DAL option '%s' (size %zu) -> ret 0x%llX, value 0x%X", key, size, ret, first);
            logged += 1;
        }
    }
    return ret;
}

// -- 3. MPC split pipes --

// Lets the DAL pick its voltage level, then withdraws every MPC split request, so each stream stays on one pipe.
int BC250DCN::wrapApplyPipeSplitFlags(void* dc, void* context, int vlevel, int* split, bool* merge)
{
    auto&      s   = singleton();
    const auto ret = FunctionCast(wrapApplyPipeSplitFlags, s.orgApplyPipeSplitFlags)(dc, context, vlevel, split, merge);
    if (split != nullptr) {
        for (size_t i = 0; i < MAX_PIPES; i++) {
            if (split[i] != 0) {
                split[i] = 0;
                s.pipeRemaps += 1;
            }
        }
    }
    return ret;
}

// pipe_ctx[] is the first member of resource_context, so an index follows from the pointer difference. On the
// first split the partner is pipe 5 (the DAL searches downwards from pipe_count - 1), which gives the stride.
void* BC250DCN::wrapFindSecondaryPipe(void* dc, void* resCtx, const void* pool, const void* primary)
{
    auto&      s   = singleton();
    auto*      ret = FunctionCast(wrapFindSecondaryPipe, s.orgFindSecondaryPipe)(dc, resCtx, pool, primary);
    if (ret == nullptr || resCtx == nullptr || ret <= resCtx) { return ret; }
    const auto delta = static_cast<size_t>(static_cast<UInt8*>(ret) - static_cast<UInt8*>(resCtx));
    if (s.pipeCtxSize == 0) {
        if (primary != resCtx || delta % 5 != 0 || delta / 5 < 0x200 || delta / 5 > 0x8000) {
            BCLOG("BC250DCN", "find_secondary_pipe: cannot derive pipe_ctx size (delta 0x%zX)", delta);
            return ret;
        }
        s.pipeCtxSize = delta / 5;
        BCLOG("BC250DCN", "pipe_ctx size 0x%zX", s.pipeCtxSize);
    }
    if (delta % s.pipeCtxSize != 0) { return ret; }
    const auto index = delta / s.pipeCtxSize;
    if (index < 4 || index > 5) { return ret; }
    s.pipeRemaps += 1;
    if (s.pipeRemaps <= 4) { BCLOG("BC250DCN", "MPC split: pipe %zu -> pipe %zu", index, index - 2); }
    return static_cast<UInt8*>(resCtx) + (index - 2) * s.pipeCtxSize;
}

// -- 2. DENTIST clocks --

// The DAL wanted appleVco / div; give it at least that from the real VCO, never above the GOP's 0x0A.
UInt32 BC250DCN::rescaleDid(UInt32 did) const
{
    if (did < 8 || did > DENTIST_DID_MAX) { return did; }
    const auto div4   = didToDiv4(did);
    const auto target = static_cast<UInt32>(static_cast<UInt64>(div4) * this->vcoKHz / this->appleVcoKHz);
    auto       newDid = div4ToDid(target);
    if (newDid < DENTIST_DID_MIN) { newDid = DENTIST_DID_MIN; }
    if (newDid > DENTIST_DID_MAX) { newDid = DENTIST_DID_MAX; }
    return newDid;
}

void BC250DCN::clockTick()
{
    auto&      nred = NRed::singleton();
    const auto v    = nred.readReg32(DENTIST_DISPCLK_CNTL);
    const auto disp = v & DENTIST_DISPCLK_WDIVIDER_MASK;
    const auto dpp  = (v >> DENTIST_DPPCLK_WDIVIDER_SHIFT) & DENTIST_DISPCLK_WDIVIDER_MASK;
    if (disp == this->lastDispDid && dpp == this->lastDppDid) { return; }

    // Only a field the DAL changed is rescaled; one still holding our last value is already corrected (the DAL
    // writes DISPCLK and DPPCLK separately, so a tick can land between the two).
    const auto newDisp = disp != this->lastDispDid ? this->rescaleDid(disp) : disp;
    auto       newDpp  = dpp != this->lastDppDid ? this->rescaleDid(dpp) : dpp;
    // Without MPC splits one pipe carries the whole stream: keep DPPCLK at least DISPCLK (a lower DID is faster).
    if (newDpp > newDisp && newDisp >= DENTIST_DID_MIN && newDpp <= DENTIST_DID_MAX) { newDpp = newDisp; }
    if (newDisp != disp) {
        nred.writeReg32(DENTIST_DISPCLK_CNTL, (nred.readReg32(DENTIST_DISPCLK_CNTL) & ~DENTIST_DISPCLK_WDIVIDER_MASK) |
                                                  newDisp);
        for (int i = 0; i < 100 && !(nred.readReg32(DENTIST_DISPCLK_CNTL) & DENTIST_DISPCLK_CHG_DONE); i++) {
            IODelay(10);
        }
    }
    if (newDpp != dpp) {
        const auto mask = DENTIST_DISPCLK_WDIVIDER_MASK << DENTIST_DPPCLK_WDIVIDER_SHIFT;
        nred.writeReg32(DENTIST_DISPCLK_CNTL,
            (nred.readReg32(DENTIST_DISPCLK_CNTL) & ~mask) | (newDpp << DENTIST_DPPCLK_WDIVIDER_SHIFT));
        for (int i = 0; i < 100 && !(nred.readReg32(DENTIST_DISPCLK_CNTL) & DENTIST_DPPCLK_CHG_DONE); i++) {
            IODelay(10);
        }
    }
    this->lastDispDid = newDisp;
    this->lastDppDid  = newDpp;
    if (newDisp != disp || newDpp != dpp) {
        this->clockFixes += 1;
        BCLOG("BC250DCN", "DENTIST DID disp 0x%X -> 0x%X, dpp 0x%X -> 0x%X", disp, newDisp, dpp, newDpp);
    }
}

void BC250DCN::clockWatcherMain(void*, int)
{
    while (true) {
        singleton().clockTick();
        IOSleep(5);
    }
}

void BC250DCN::startClockWatcher()
{
    thread_t thread = nullptr;
    if (kernel_thread_start(reinterpret_cast<thread_continue_t>(clockWatcherMain), this, &thread) != KERN_SUCCESS) {
        BCLOG("BC250DCN", "Failed to start the DENTIST watcher");
        return;
    }
    thread_deallocate(thread);
}
