// ASRock BC-250 (AMD Cyan Skillfish) Support
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.
//
// The probe only reads hardware state. The only register writes are the index registers of indirect access
// pairs (MM_INDEX/MM_INDEX_HI, PCIE_INDEX2) and GRBM_GFX_INDEX (restored to broadcast afterwards), which is what
// amdgpu does during init too. It never talks to the SMU or PSP mailboxes.

#include <AppleACPIPlatformExpert.hpp>
#include <BC250.hpp>
#include <BC250HWL.hpp>
#include <BC250Smu.hpp>
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <NRed.hpp>
#include <Regs/CyanSkillfish.hpp>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <sys/sysctl.h>

static BC250 moduleInstance;

SYSCTL_NODE(_debug, OID_AUTO, bc250, CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, "BC-250");

static char         bc250Log[1024 * 1024];
static volatile SInt32 bc250LogPos = 0;

void bc250LogAppend(const char* module, const char* format, ...)
{
    char    line[512];
    auto    len = snprintf(line, sizeof(line), "[%llu] %s: ", mach_absolute_time() / 1000000, module);
    va_list args;
    va_start(args, format);
    len += vsnprintf(line + len, sizeof(line) - static_cast<size_t>(len) - 1, format, args);
    va_end(args);
    if (len > static_cast<int>(sizeof(line)) - 2) { len = sizeof(line) - 2; }
    line[len++] = '\n';
    const auto pos = OSAddAtomic(len, &bc250LogPos);
    if (pos >= 0 && pos + len < static_cast<SInt32>(sizeof(bc250Log))) { memcpy(bc250Log + pos, line, len); }
}

static int sysctlHandleLog(struct sysctl_oid*, void*, int, struct sysctl_req* req)
{
    auto len = bc250LogPos;
    if (len >= static_cast<SInt32>(sizeof(bc250Log))) { len = sizeof(bc250Log) - 1; }
    bc250Log[len] = '\0';
    return SYSCTL_OUT(req, bc250Log, static_cast<size_t>(len) + 1);
}

SYSCTL_PROC(_debug_bc250, OID_AUTO, log, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, sysctlHandleLog, "A",
            "BC-250 log since boot");

// SDMA1's trap enable follows SDMA0's (BC250HWL::sdma1TrapPoll), checked every 0.2 s on a thread of its own.
static void bc250PollThread(void*, wait_result_t)
{
    IOSleep(2000);
    while (true) {
        BC250HWL::singleton().sdma1TrapPoll();
        IOSleep(200);
    }
}

static void bc250StartPollThread()
{
    static bool started = false;
    if (started) { return; }
    thread_t thread = nullptr;
    if (kernel_thread_start(bc250PollThread, nullptr, &thread) != KERN_SUCCESS) { return; }
    thread_deallocate(thread);
    started = true;
}

static void registerSysctls()
{
    static bool registered = false;
    if (registered) { return; }
    registered = true;
    sysctl_register_oid(&sysctl__debug_bc250);
    sysctl_register_oid(&sysctl__debug_bc250_log);
}

BC250& BC250::singleton() { return moduleInstance; }

namespace
{
    constexpr UInt32 GRBM_GFX_INDEX           = CyanSkillfish::GC_BASE_1 + 0x2200;
    constexpr UInt32 GRBM_GFX_INDEX_BROADCAST = 0xE0000000;    // SE/SA/instance broadcast.
    constexpr UInt32 GRBM_GFX_INDEX_INSTANCE_BROADCAST = 0x40000000;

    constexpr UInt32 BINARY_SIGNATURE          = 0x28211407;
    constexpr UInt32 DISCOVERY_TABLE_SIGNATURE = 0x53445049;
    constexpr UInt32 GC_TABLE_ID               = 0x4347;

    // `soc15_hw_ip.h`
    constexpr UInt16 MP1_HWID    = 1;
    constexpr UInt16 GC_HWID     = 11;
    constexpr UInt16 UVD_HWID    = 12;    // VCN
    constexpr UInt16 SDMA0_HWID  = 42;
    constexpr UInt16 SDMA1_HWID  = 43;
    constexpr UInt16 DMU_HWID    = 271;
    constexpr UInt16 MP0_HWID    = 255;

    template<typename T>
    bool readAt(const UInt8* const bin, const size_t size, const size_t off, T& out)
    {
        if (off > size || sizeof(T) > size - off) { return false; }
        memcpy(&out, bin + off, sizeof(T));
        return true;
    }

    UInt32 popCount(UInt32 v)
    {
        UInt32 n = 0;
        for (; v != 0; v &= v - 1) { n += 1; }
        return n;
    }

    void setNumber(OSDictionary* const dict, const char* const key, const UInt64 value, const UInt32 bits = 32)
    {
        auto* const num = OSNumber::withNumber(value, bits);
        if (num == nullptr) { return; }
        dict->setObject(key, num);
        num->release();
    }

    void setData(OSDictionary* const dict, const char* const key, const void* const bytes, const size_t size)
    {
        auto* const data = OSData::withBytes(bytes, static_cast<unsigned int>(size));
        if (data == nullptr) { return; }
        dict->setObject(key, data);
        data->release();
    }
}    // namespace

void BC250::processPatcher()
{
    registerSysctls();

    this->probe();
    this->active = this->info.carveOutMiB != 0;
    if (this->active) {
        bc250StartPollThread();
    }
    else {
        BCLOG("BC250", "Could not read the VRAM carve-out size; leaving the GPU to the firmware framebuffer.");
    }

    // SMU telemetry (read-only messages; bc250smu=0 disables): GPU/CPU clocks, voltages, Tctl, core mask.
    BC250Smu::singleton().start();

}

void BC250::probe()
{
    NRed::singleton().hwLateInit();

    auto* const dict = OSDictionary::withCapacity(64);
    if (dict == nullptr) {
        BCLOG("BC250", "Failed to allocate the probe dictionary.");
        return;
    }

    this->probeRegisters(dict);
    this->probeDiscovery(dict);
    this->probeVBIOS(dict);



    NRed::singleton().getIGPU()->setProperty("BC250,probe", dict);
    dict->release();
}

void BC250::probeRegisters(OSDictionary* const dict)
{
    using namespace CyanSkillfish;
    auto& nred = NRed::singleton();
    auto* dev  = nred.getIGPU();

    const auto bar0 = dev->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    const auto bar2 = dev->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    this->info.bar0Size = bar0 == nullptr ? 0 : bar0->getLength();
    setNumber(dict, "bar0-size", this->info.bar0Size, 64);
    setNumber(dict, "bar2-size", bar2 == nullptr ? 0 : bar2->getLength(), 64);
    setNumber(dict, "bar5-size", nred.getMMIOLength(), 64);

    const auto memsize = nred.readReg32(RCC_CONFIG_MEMSIZE);
    this->info.carveOutMiB = (memsize == 0xFFFFFFFF) ? 0 : memsize;
    setNumber(dict, "carve-out-mib", this->info.carveOutMiB);

    const auto strap = nred.readReg32(RCC_DEV0_EPF0_STRAP0);
    setNumber(dict, "rcc-strap0", strap);
    setNumber(dict, "dev-revision", nred.getDevRevision());
    setNumber(dict, "pci-revision", nred.getPciRevision());

    const auto fbBase = nred.readReg32(GCMC_VM_FB_LOCATION_BASE);
    const auto fbTop  = nred.readReg32(GCMC_VM_FB_LOCATION_TOP);
    this->info.fbLocationBase = static_cast<UInt64>(fbBase & 0xFFFFFF) << 24;
    this->info.fbLocationTop  = (static_cast<UInt64>(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF;
    setNumber(dict, "fb-location-base", this->info.fbLocationBase, 64);
    setNumber(dict, "fb-location-top", this->info.fbLocationTop, 64);
    setNumber(dict, "fb-offset", nred.getFbOffset(), 64);
    setNumber(dict, "agp-base", nred.readReg32(GCMC_VM_AGP_BASE));
    setNumber(dict, "agp-bot", nred.readReg32(GCMC_VM_AGP_BOT));
    setNumber(dict, "agp-top", nred.readReg32(GCMC_VM_AGP_TOP));

    this->info.gbAddrConfig = nred.readReg32(GB_ADDR_CONFIG);
    setNumber(dict, "gb-addr-config", this->info.gbAddrConfig);
    setNumber(dict, "cc-rb-backend-disable", nred.readReg32(CC_RB_BACKEND_DISABLE));
    setNumber(dict, "grbm-status", nred.readReg32(GRBM_STATUS));

    const auto psp33  = nred.readReg32(MP0_SMN_C2PMSG_33);
    const auto psp58  = nred.readReg32(MP0_SMN_C2PMSG_58);
    const auto psp81  = nred.readReg32(MP0_SMN_C2PMSG_81);
    const auto psp100 = nred.readReg32(MP0_SMN_C2PMSG_100);
    setNumber(dict, "psp-c2pmsg-33", psp33);
    setNumber(dict, "psp-c2pmsg-58", psp58);
    setNumber(dict, "psp-c2pmsg-81", psp81);
    setNumber(dict, "psp-c2pmsg-100", psp100);

    const auto smuFlags = nred.readSMN32(SMN_MP1_FIRMWARE_FLAGS);
    this->info.smuFirmwareRunning = (smuFlags & MP1_FIRMWARE_FLAGS_INTERRUPTS_ENABLED) != 0;
    setNumber(dict, "smu-firmware-flags", smuFlags);
    setNumber(dict, "smu-c2pmsg-66", nred.readReg32(MP1_SMN_C2PMSG_66));
    setNumber(dict, "smu-c2pmsg-82", nred.readReg32(MP1_SMN_C2PMSG_82));
    setNumber(dict, "smu-c2pmsg-90", nred.readReg32(MP1_SMN_C2PMSG_90));


}

void BC250::probeDiscovery(OSDictionary* const dict)
{
    using namespace CyanSkillfish;
    if (this->info.carveOutMiB == 0) {
        BCLOG("BC250", "Skipping IP discovery: carve-out size unknown.");
        return;
    }

    const UInt64 offset = (static_cast<UInt64>(this->info.carveOutMiB) << 20) - DISCOVERY_TMR_OFFSET;
    UInt32       signature;
    if (!NRed::singleton().readVRAM(offset, &signature, sizeof(signature)) || signature != BINARY_SIGNATURE) {
        BCLOG("BC250", "No IP discovery binary at VRAM offset 0x%llX (read 0x%X).", offset, signature);
        return;
    }

    auto* const bin = static_cast<UInt8*>(IOMalloc(DISCOVERY_TMR_SIZE));
    if (bin == nullptr) { return; }
    if (NRed::singleton().readVRAM(offset, bin, DISCOVERY_TMR_SIZE)) {
        this->info.hasDiscovery = true;
        setData(dict, "ip-discovery", bin, DISCOVERY_TMR_SIZE);
        this->parseDiscovery(bin, DISCOVERY_TMR_SIZE, dict);
    }
    IOFree(bin, DISCOVERY_TMR_SIZE);
}

void BC250::parseDiscovery(const UInt8* const bin, const size_t size, OSDictionary* const dict)
{
    UInt16 verMajor = 0, verMinor = 0;
    readAt(bin, size, 4, verMajor);
    readAt(bin, size, 6, verMinor);
    setNumber(dict, "ip-discovery-version", (static_cast<UInt32>(verMajor) << 16) | verMinor);

    // binary_header (v1) has 6 fixed table_info entries at 0xC; binary_header_v2 has num_tables at 0xC.
    size_t tableListOff = 0xC;
    UInt16 numTables    = 6;
    if (verMajor >= 2) {
        readAt(bin, size, 0xC, numTables);
        tableListOff = 0x10;
    }

    UInt16 ipTableOff = 0, gcTableOff = 0;
    if (numTables > 0) { readAt(bin, size, tableListOff + 0 * 8, ipTableOff); }
    if (numTables > 1) { readAt(bin, size, tableListOff + 1 * 8, gcTableOff); }

    // IP list.
    UInt32 ipSig = 0;
    UInt16 ipVer = 0, numDies = 0;
    if (ipTableOff != 0 && readAt(bin, size, ipTableOff, ipSig) && ipSig == DISCOVERY_TABLE_SIGNATURE) {
        readAt(bin, size, ipTableOff + 4, ipVer);
        readAt(bin, size, ipTableOff + 12, numDies);
        UInt8 flags = 0;
        readAt(bin, size, ipTableOff + 78, flags);
        const bool base64 = ipVer == 4 && (flags & 1) != 0;

        for (UInt16 d = 0; d < numDies && d < 16; d += 1) {
            UInt16 dieOff = 0, numIps = 0;
            readAt(bin, size, ipTableOff + 14 + d * 4 + 2, dieOff);
            if (dieOff == 0 || !readAt(bin, size, dieOff + 2, numIps)) { continue; }

            size_t off = dieOff + 4;
            for (UInt16 i = 0; i < numIps; i += 1) {
                UInt16 hwId = 0;
                UInt8  inst = 0, numBase = 0, major = 0, minor = 0, rev = 0, misc = 0;
                if (!readAt(bin, size, off, hwId) || !readAt(bin, size, off + 2, inst) ||
                    !readAt(bin, size, off + 3, numBase) || !readAt(bin, size, off + 4, major) ||
                    !readAt(bin, size, off + 5, minor) || !readAt(bin, size, off + 6, rev) ||
                    !readAt(bin, size, off + 7, misc))
                {
                    break;
                }
                UInt32 base0 = 0;
                readAt(bin, size, off + 8, base0);
                // v1/v2: low nibble is the harvest flag (1 = harvested). v3+: sub-revision/variant.
                const bool harvested = ipVer < 3 && (misc & 0xF) == 1;


                if (!harvested) {
                    switch (hwId) {
                        case GC_HWID:
                            this->info.gcMajor    = major;
                            this->info.gcMinor    = minor;
                            this->info.gcRevision = rev;
                            break;
                        case DMU_HWID:
                            this->info.dcnMajor    = major;
                            this->info.dcnMinor    = minor;
                            this->info.dcnRevision = rev;
                            break;
                        case SDMA0_HWID:
                        case SDMA1_HWID:
                            this->info.sdmaCount += 1;
                            break;
                        case UVD_HWID:
                            this->info.hasVCN = true;
                            break;
                        case MP0_HWID:
                        case MP1_HWID:
                        default:
                            break;
                    }
                }

                off += 8 + static_cast<size_t>(numBase) * (base64 ? 8 : 4);
            }
        }
    }
    else {
        BCLOG("BC250", "IP discovery: IP table missing or bad signature (off 0x%X sig 0x%X).", ipTableOff, ipSig);
    }

    setNumber(dict, "gc-version",
              (static_cast<UInt32>(this->info.gcMajor) << 16) | (static_cast<UInt32>(this->info.gcMinor) << 8) |
                  this->info.gcRevision);
    setNumber(dict, "dcn-version",
              (static_cast<UInt32>(this->info.dcnMajor) << 16) | (static_cast<UInt32>(this->info.dcnMinor) << 8) |
                  this->info.dcnRevision);
    setNumber(dict, "sdma-count", this->info.sdmaCount);
    setNumber(dict, "has-vcn", this->info.hasVCN ? 1 : 0);

    // GC info: shader engine layout.
    UInt32 gcId = 0;
    if (gcTableOff == 0 || !readAt(bin, size, gcTableOff, gcId) || gcId != GC_TABLE_ID) { return; }
    UInt16 gcVerMajor = 0, gcVerMinor = 0;
    readAt(bin, size, gcTableOff + 4, gcVerMajor);
    readAt(bin, size, gcTableOff + 6, gcVerMinor);

    // gc_info_v1_0 follows the 12-byte gpu_info_header.
    const size_t gc = gcTableOff + 12;
    UInt32       numSe = 0, wgp0PerSa = 0, wgp1PerSa = 0, rbPerSe = 0, gl2c = 0, waveSize = 0, ldsSize = 0,
           saPerSe  = 0;
    readAt(bin, size, gc + 0 * 4, numSe);
    readAt(bin, size, gc + 1 * 4, wgp0PerSa);
    readAt(bin, size, gc + 2 * 4, wgp1PerSa);
    readAt(bin, size, gc + 3 * 4, rbPerSe);
    readAt(bin, size, gc + 4 * 4, gl2c);
    readAt(bin, size, gc + 11 * 4, waveSize);
    readAt(bin, size, gc + 14 * 4, ldsSize);
    readAt(bin, size, gc + 16 * 4, saPerSe);

    setNumber(dict, "gc-num-se", numSe);
    setNumber(dict, "gc-num-sa-per-se", saPerSe);
    setNumber(dict, "gc-num-wgp0-per-sa", wgp0PerSa);
    setNumber(dict, "gc-num-wgp1-per-sa", wgp1PerSa);
    setNumber(dict, "gc-num-rb-per-se", rbPerSe);

    // Active CUs: walk every SE/SA like `gfx_v10_0_get_cu_info`, then restore broadcast.
    const auto wgpPerSa = wgp0PerSa + wgp1PerSa;
    if (numSe == 0 || numSe > 4 || saPerSe == 0 || saPerSe > 4 || wgpPerSa == 0 || wgpPerSa > 16) { return; }

    auto&        nred    = NRed::singleton();
    const UInt32 wgpMask = (1U << wgpPerSa) - 1;
    UInt32       activeWgps = 0;
    for (UInt32 se = 0; se < numSe; se += 1) {
        for (UInt32 sa = 0; sa < saPerSe; sa += 1) {
            nred.writeReg32(GRBM_GFX_INDEX, (se << 16) | (sa << 8) | GRBM_GFX_INDEX_INSTANCE_BROADCAST);
            const auto inactive =
                (nred.readReg32(CyanSkillfish::CC_GC_SHADER_ARRAY_CONFIG) |
                 nred.readReg32(CyanSkillfish::GC_USER_SHADER_ARRAY_CONFIG)) >>
                CyanSkillfish::SHADER_ARRAY_INACTIVE_WGPS_SHIFT;
            const auto active = ~inactive & wgpMask;
            activeWgps += popCount(active);
        }
    }
    nred.writeReg32(GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);

    this->info.activeCUs = activeWgps * 2;
    setNumber(dict, "active-cus", this->info.activeCUs);
}

void BC250::probeVBIOS(OSDictionary* const dict)
{
    auto* const dev    = NRed::singleton().getIGPU();
    auto* const expert = static_cast<AppleACPIPlatformExpert*>(dev->getPlatform());
    if (expert == nullptr) { return; }

    const auto* const vfctData = expert->getACPITableData("VFCT", 0);
    if (vfctData == nullptr || vfctData->getLength() < sizeof(VFCT)) {
        BCLOG("BC250", "No usable ACPI VFCT table; macOS will have to find the VBIOS elsewhere.");
        return;
    }
    const auto* const vfct = static_cast<const VFCT*>(vfctData->getBytesNoCopy());

    const UInt8* image     = nullptr;
    UInt32       imageSize = 0;
    for (auto off = vfct->vbiosImageOffset; off + sizeof(GOPVideoBIOSHeader) <= vfctData->getLength();) {
        const auto* const hdr =
            static_cast<const GOPVideoBIOSHeader*>(vfctData->getBytesNoCopy(off, sizeof(GOPVideoBIOSHeader)));
        if (hdr == nullptr || hdr->imageLength == 0) { break; }
        const auto* const content =
            static_cast<const UInt8*>(vfctData->getBytesNoCopy(off + sizeof(GOPVideoBIOSHeader), hdr->imageLength));
        if (content != nullptr && hdr->vendorID == 0x1002 && hdr->deviceID == NRed::singleton().getDeviceID()) {
            image     = content;
            imageSize = hdr->imageLength;
            break;
        }
        off += sizeof(GOPVideoBIOSHeader) + hdr->imageLength;
    }

    if (image == nullptr || imageSize < 0x100 || image[0] != 0x55 || image[1] != 0xAA) {
        BCLOG("BC250", "No valid VBIOS image for this GPU in VFCT.");
        return;
    }
    this->info.hasVBIOS = true;
    setData(dict, "vbios", image, imageSize);

    UInt16 romHdr = 0, mdt = 0, mdtSize = 0;
    readAt(image, imageSize, ATOM_ROM_TABLE_PTR, romHdr);
    UInt32 atomSigRaw = 0;
    if (romHdr == 0 || !readAt(image, imageSize, romHdr + 4, atomSigRaw)) { return; }
    char atomSig[5] = {};
    memcpy(atomSig, &atomSigRaw, sizeof(atomSigRaw));
    readAt(image, imageSize, romHdr + ATOM_ROM_DATA_PTR, mdt);
    readAt(image, imageSize, mdt, mdtSize);

    // Indices into atom_master_list_of_data_tables_v2_1 that the X6000FB path cares about.
    static constexpr struct
    {
        UInt32      index;
        const char* name;
    } tables[] = {
        {0x04, "firmwareinfo"}, {0x09, "psp-directory"},        {0x0F, "powerplayinfo"},
        {0x16, "displayobjectinfo"}, {0x1C, "vram_info"}, {0x1E, "integratedsysteminfo"},
    };
    for (const auto& t : tables) {
        UInt16 tableOff = 0;
        readAt(image, imageSize, mdt + sizeof(ATOMCommonTableHeader) + t.index * 2, tableOff);
        ATOMCommonTableHeader hdr {};
        if (tableOff != 0) { readAt(image, imageSize, tableOff, hdr); }
        char key[48];
        snprintf(key, sizeof(key), "atom-%s", t.name);
        setNumber(dict, key, (static_cast<UInt32>(tableOff) << 16) | (hdr.formatRev << 8) | hdr.contentRev);
    }
}
