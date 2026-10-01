// AMDRadeonX6000Framebuffer Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "AmdAtomPspDirectoryDummy.hpp"
#include "AmdAtomVramInfoIGP.hpp"
#include <AppleACPIPlatformExpert.hpp>
#include <BC250.hpp>
#include <BC250DCN.hpp>
#include <ASICCaps.hpp>
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <GPUDriversAMD/CAIL/ASICCaps.hpp>
#include <GPUDriversAMD/FB/AmdAsicInfo.hpp>
#include <GPUDriversAMD/FB/AmdDeviceMemoryManager.hpp>
#include <GPUDriversAMD/FB/VidMemType.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <Regs/OSSSYS_4.hpp>
#include <Regs/SMUIO.hpp>
#include <X6000FB.hpp>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_param.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const UInt8 kCailAsicCapsTablePattern[] = {0x6E, 0x00, 0x00, 0x00, 0x98, 0x67, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                                  0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPopulateVramInfoPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48,
                                                     0x81, 0xEC, 0x08, 0x01, 0x00, 0x00, 0x40, 0x89, 0xF0, 0x40,
                                                     0x89, 0xF0, 0x4C, 0x8D, 0xBD, 0xE0, 0xFE, 0xFF, 0xFF};
static const UInt8 kPopulateVramInfoPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xF0, 0xF0,
                                                     0xFF, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const UInt8      kCreateVramInfoCallPattern[]          = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                 0xB7, 0x70, 0x3C, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreateVramInfoCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreateVramInfoCallPatternJumpInstOff = 12;

static const UInt8      kCreatePspDirectoryCallPattern[]     = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                0xB7, 0x70, 0x16, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreatePspDirectoryCallPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreatePspDirectoryCallPatternJumpInstOff = 12;

static const UInt8      kCreateObjectInfoCallPattern[]          = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                   0xB7, 0x70, 0x30, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreateObjectInfoCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                   0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreateObjectInfoCallPatternJumpInstOff = 12;

// Change cursor and underflow tracker count to 4 instead of 6.
static const UInt8 kCreateControllerServicesOriginal[]     = {0x40, 0x00, 0x00, 0x40, 0x83, 0x00, 0x06};
static const UInt8 kCreateControllerServicesOriginalMask[] = {0xF0, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kCreateControllerServicesPatched[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
static const UInt8 kCreateControllerServicesPatchedMask[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F};

// Ditto, 10.15.
static const UInt8 kCreateControllerServicesOriginal1015[]     = {0x48, 0x00, 0x00, 0x48, 0x83, 0x00, 0x05};
static const UInt8 kCreateControllerServicesOriginalMask1015[] = {0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF};
static const UInt8 kCreateControllerServicesPatched1015[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
static const UInt8 kCreateControllerServicesPatchedMask1015[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F};

// Change cursor count to 4 instead of 6.
static const UInt8 kSetupCursorsOriginal[]     = {0x40, 0x83, 0x00, 0x05};
static const UInt8 kSetupCursorsOriginalMask[] = {0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kSetupCursorsPatched[]      = {0x00, 0x00, 0x00, 0x03};
static const UInt8 kSetupCursorsPatchedMask[]  = {0x00, 0x00, 0x00, 0x0F};

// Ditto, 12.0+.
static const UInt8 kSetupCursorsOriginal12[]     = {0x40, 0x83, 0x00, 0x06};
static const UInt8 kSetupCursorsOriginalMask12[] = {0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kSetupCursorsPatched12[]      = {0x00, 0x00, 0x00, 0x04};
static const UInt8 kSetupCursorsPatchedMask12[]  = {0x00, 0x00, 0x00, 0x0F};

// Change link count to 4 instead of 6.
static const UInt8 kCreateLinksOriginal[]     = {0x06, 0x00, 0x00, 0x00, 0x40};
static const UInt8 kCreateLinksOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
static const UInt8 kCreateLinksPatched[]      = {0x04, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kCreateLinksPatchedMask[]  = {0x0F, 0x00, 0x00, 0x00, 0x00};

// Remove new FB count condition so we can restore the original behaviour before Ventura.
static const UInt8 kControllerPowerUpOriginal[]     = {0x38, 0xC8, 0x0F, 0x42, 0xC8, 0x88, 0x8F,
                                                       0xBC, 0x00, 0x00, 0x00, 0x72, 0x00};
static const UInt8 kControllerPowerUpOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static const UInt8 kControllerPowerUpReplace[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x00, 0x00, 0xEB, 0x00};
static const UInt8 kControllerPowerUpReplaceMask[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x00, 0x00, 0xFF, 0x00};

// Remove new problematic Ventura pixel clock multiplier calculation which causes timing validation mishaps.
static const UInt8 kValidateDetailedTimingOriginal[] = {0x66, 0x0F, 0x2E, 0xC1, 0x76, 0x06, 0xF2, 0x0F, 0x5E, 0xC1};
static const UInt8 kValidateDetailedTimingPatched[]  = {0x66, 0x0F, 0x2E, 0xC1, 0x66, 0x90, 0xF2, 0x0F, 0x5E, 0xC1};

static X6000FB moduleInstance;

X6000FB& X6000FB::singleton() { return moduleInstance; }

void X6000FB::processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
{
    if (kextRadeonX6000Framebuffer.loadIndex != id) { return; }

    if (BC250::singleton().isActive()) { this->processKextCyanSkillfish(patcher, id, slide, size); }
}

UInt16 X6000FB::getEnumeratedRevision() { return NRed::singleton().getEnumRevision(); }

enum IRQMgrIVRingMemoryType
{
    IRQMgrIVRingMemoryTypeGART        = 0x0,
    IRQMgrIVRingMemoryTypeSysPhysical = 0x1,
    IRQMgrIVRingMemoryTypeFB          = 0x2,
};

IOReturn X6000FB::initialiseReservedVRAM(void* const self)
{
#define CHECK(_expr)                                                     \
    if (const auto ret = _expr; ret != kIOReturnSuccess) { return ret; }
    static constexpr IOOptionBits mapOptions = kIOMapWriteCombineCache | kIOMapAnywhere;
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor1_32bpp, 0, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor1_2bpp, 0x40000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor2_32bpp, 0x80000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor2_2bpp, 0xC0000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor3_32bpp, 0x100000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor3_2bpp, 0x140000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor4_32bpp, 0x180000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor4_2bpp, 0x1C0000, 0x40000, mapOptions));
    CHECK(
        singleton().mapMemorySubRange(self, AmdReservedMemorySelector::PPLIBReserved, 0x200000, 0x100000, mapOptions));
    if (NRed::singleton().getAttributes().isRenoir()) {
        CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::DMCUBReserved, 0x300000, 0x100000,
                                            mapOptions));
        return singleton().mapMemorySubRange(self, AmdReservedMemorySelector::ReserveVRAM, 0, 0x400000, mapOptions);
    }
    return singleton().mapMemorySubRange(self, AmdReservedMemorySelector::ReserveVRAM, 0, 0x300000, mapOptions);
#undef CHECK
}

IOReturn X6000FB::dummyIOReturnSuccess() { return kIOReturnSuccess; }

UInt32 X6000FB::wrapControllerPowerUp(void* const self)
{
    auto& m_flags  = getMember<UInt8>(self, 0x5F18);
    auto  send     = (m_flags & 2) == 0;
    m_flags       |= 4;    // All framebuffers enabled
    auto ret       = FunctionCast(wrapControllerPowerUp, singleton().orgControllerPowerUp)(self);
    if (send) { singleton().orgMessageAccelerator(self, IOFBRequestControllerEnabled, nullptr, nullptr, nullptr); }
    return ret;
}

void* X6000FB::wrapCreateObjectInfo(void* const helper, const UInt32 tableOffset)
{
    const auto ret = FunctionCast(wrapCreateObjectInfo, singleton().orgCreateObjectInfo)(helper, tableOffset);
    if (ret == nullptr) { return ret; }

    const auto infoTable = getMember<DispObjInfoTableV1*>(ret, 0x28);
    const auto n         = infoTable->pathCount;
    for (UInt8 i = 0, j = 0; i < n; i++) {
        // Skip invalid device tags
        if (infoTable->paths[i].devTag == 0) { infoTable->pathCount--; }
        else {
            infoTable->paths[j++] = infoTable->paths[i];
        }
    }

    return ret;
}

static UInt32 getTableOffset(const AmdAtomFwHelper* const biosHelper, const UInt32 index)
{
    const auto romTableOffset = static_cast<const UInt16*>(biosHelper->getImage(ATOM_ROM_TABLE_PTR, sizeof(UInt16)));
    if (romTableOffset == nullptr) { return 0; }
    const auto mdtOffset =
        static_cast<const UInt16*>(biosHelper->getImage(*romTableOffset + ATOM_ROM_DATA_PTR, sizeof(UInt32)));
    if (mdtOffset == nullptr) { return 0; }
    const auto mdt =
        static_cast<const UInt8*>(biosHelper->getImage(*mdtOffset, /*sizeof(atom_master_data_table_v2_1)*/ 0x4A));
    if (mdt == nullptr) { return 0; }
    return reinterpret_cast<const UInt16*>(mdt + sizeof(ATOMCommonTableHeader))[index];
}

AmdAtomVramInfo* X6000FB::wrapCreateVramInfo(AmdAtomFwHelper* const biosHelper, const UInt32 tableOffset)
{
    if (biosHelper == nullptr || tableOffset != 0) {
        return FunctionCast(wrapCreateVramInfo, singleton().orgCreateVramInfo)(biosHelper, tableOffset);
    }
    return AmdAtomVramInfoIGP::createVramInfoIGP(biosHelper, getTableOffset(biosHelper, 0x1E));
}

IOReturn X6000FB::wrapPopulateVramInfo(AmdAtomVramInfo* const self, AtomFirmwareInfo& fwInfo)
{ return self->populateVramInfo(fwInfo); }

IOReturn X6000FB::wrapGetVendorInfo(const void* const self, AGDCVendorInfo_t* const vendorInfo,
                                    const size_t sizeofVendorInfo)
{
    const auto ret = FunctionCast(wrapGetVendorInfo, singleton().orgGetVendorInfo)(self, vendorInfo, sizeofVendorInfo);
    if (ret == kIOReturnSuccess) [[likely]] { vendorInfo->VendorClass = kAGDCVendorClassIntegratedGPU; }
    return ret;
}

size_t X6000FB::readVfctAtomBiosImage(void* const self, UInt8* const buffer, const size_t bufferSize, const bool strict)
{
    const auto pciDevice = getMember<IOPCIDevice*>(self, 0x28);

    const auto expert = static_cast<AppleACPIPlatformExpert*>(pciDevice->getPlatform());
    if (expert == nullptr) [[unlikely]] { return 0; }

    const auto vfctData = expert->getACPITableData("VFCT", 0);
    if (vfctData == nullptr) [[unlikely]] { return 0; }

    const auto vfct = static_cast<const VFCT*>(vfctData->getBytesNoCopy());
    if (vfct == nullptr) [[unlikely]] { return 0; }

    if (sizeof(VFCT) > vfctData->getLength()) [[unlikely]] { return 0; }

    const auto deviceID = pciDevice->extendedConfigRead16(kIOPCIConfigDeviceID);
    const auto vendor   = pciDevice->extendedConfigRead16(kIOPCIConfigVendorID);
    const auto busNum   = pciDevice->getBusNumber();
    const auto devNum   = pciDevice->getDeviceNumber();
    const auto devFunc  = pciDevice->getFunctionNumber();

    for (auto offset = vfct->vbiosImageOffset; offset < vfctData->getLength();) {
        auto vHdr =
            static_cast<const GOPVideoBIOSHeader*>(vfctData->getBytesNoCopy(offset, sizeof(GOPVideoBIOSHeader)));
        if (vHdr == nullptr) [[unlikely]] { return 0; }

        const auto vContent =
            static_cast<const UInt8*>(vfctData->getBytesNoCopy(offset + sizeof(GOPVideoBIOSHeader), vHdr->imageLength));
        if (vContent == nullptr) [[unlikely]] { return 0; }

        offset += sizeof(GOPVideoBIOSHeader) + vHdr->imageLength;

        if (vHdr->imageLength != 0 && vHdr->imageLength <= bufferSize
            && (!strict || (vHdr->pciBus == busNum && vHdr->pciDevice == devNum && vHdr->pciFunction == devFunc))
            && vHdr->vendorID == vendor && vHdr->deviceID == deviceID) [[likely]]
        {
            if (singleton().validateAtomBiosImage(self, const_cast<UInt8*>(vContent), vHdr->imageLength)) [[likely]] {
                memcpy(buffer, vContent, vHdr->imageLength);
                return vHdr->imageLength;
            }
            else {
                SYSLOG("X6000FB", "BIOS Validation Failed - Reading from VFCT.");
            }
        }
        else {
            SYSLOG("NRed",
                   "VFCT image does not match, is empty or too long (pciBus: 0x%X pciDevice: 0x%X pciFunction: 0x%X "
                   "vendorID: 0x%X deviceID: 0x%X imageLength: 0x%X).",
                   vHdr->pciBus, vHdr->pciDevice, vHdr->pciFunction, vHdr->vendorID, vHdr->deviceID, vHdr->imageLength);
        }
    }

    SYSLOG("NRed", "VFCT table present but broken.");
    return 0;
}

size_t X6000FB::readVramAtomBiosImage(void* const self, UInt8* const buffer, const size_t bufferSize)
{
    const auto pciDevice = getMember<IOPCIDevice*>(self, 0x28);

    const auto bar0 =
        pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapWriteCombineCache | kIOMapAnywhere);
    if (bar0 == nullptr) [[unlikely]] { return 0; }

    if (bar0->getLength() == 0) [[unlikely]] {
        bar0->release();
        return 0;
    }

    const auto fb = reinterpret_cast<UInt8*>(bar0->getVirtualAddress());

    if (singleton().validateAtomBiosImage(self, fb, bufferSize)) [[likely]] {
        memcpy(buffer, fb, bufferSize);
        bar0->release();
        return bufferSize;
    }

    SYSLOG("X6000FB", "BIOS Validation Failed - Reading from VRAM.");
    bar0->release();
    return 0;
}

// TODO: See `amdgpu_device_need_post`, `amdgpu_get_bios_dgpu`, `amdgpu_get_bios_apu`.
IOReturn X6000FB::readAtomBios(void* const self)
{
    auto& biosImage = getMember<UInt8[0x10000]>(self, 0x48);
    auto  size      = singleton().readEfiAtomBiosImage(self, biosImage, sizeof(biosImage));
    if (size == 0) [[likely]] {
        size = readVfctAtomBiosImage(self, biosImage, sizeof(biosImage));
        if (size == 0) [[unlikely]] {
            size = readVramAtomBiosImage(self, biosImage, sizeof(biosImage));
            if (size == 0) [[unlikely]] {
                size = singleton().readPciAtomBiosImage(self, biosImage, sizeof(biosImage));
                if (size == 0) [[likely]] {
                    size = readVfctAtomBiosImage(self, biosImage, sizeof(biosImage), false);
                    if (size == 0) [[unlikely]] { return kIOReturnInternalError; }
                }
                else if (!singleton().validateAtomBiosImage(self, biosImage, sizeof(biosImage))) [[unlikely]] {
                    SYSLOG("X6000FB", "BIOS Validation Failed - Reading from PCI Device.");
                    return kIOReturnInternalError;
                }
            }
        }
    }
    getMember<size_t>(self, 0x10048) = size;
    return kIOReturnSuccess;
}

AmdAtomPspDirectory* X6000FB::wrapCreatePspDirectory(AmdAtomFwHelper* const biosHelper, const UInt32 tableOffset)
{
    if (biosHelper == nullptr || tableOffset != 0) {
        return FunctionCast(wrapCreatePspDirectory, singleton().orgCreatePspDirectory)(biosHelper, tableOffset);
    }
    return AmdAtomPspDirectoryDummy::create();
}

// -- ASRock BC-250 (Cyan Skillfish, GC 10.1.3, DCN 2.01) --
//
// The BC-250 is a Navi-family (0x8F) part with a Navi 10-compatible register layout, so unlike Raven/Renoir we keep
// the Navi family, the native strap register, the native IH 5.0 and the native ROM access registers. What we still
// need from the iGPU toolbox is: VBIOS from VFCT, VRAM info from IntegratedSystemInfo when there is no vram_info
// table, the PSP directory dummy, a carve-out sized reserved VRAM layout, and the 4-pipe limit (DCN 2.01 has 4 pipes
// and 2 timing generators; Navi 10's DCN 2.0 has 6).

static const AmdAsicBrandingTableEntry bc250BrandingTable[] = {
    {"Radeon", "BC-250 Graphics"},
};

const AmdAsicBrandingTableEntry* X6000FB::getGpuBrandingNameListBC250(const void*) { return bc250BrandingTable; }

IOReturn X6000FB::getTriageHardwareDataStub(void*, UInt32, void* const triageData)
{
    // The DCN 2.0 register offsets used by the original are not valid on DCN 2.01. Report nothing.
    return getMember<UInt32>(triageData, 0x8) < 2 ? kIOReturnNoResources : kIOReturnSuccess;
}

static UInt32 bc250DdiCaps[16] = {};

// `AmdAtomPspDirectory::createPspDirectory` is stripped, and the generic jump-pattern helper rejects any call target
// at or past `slide + size`. On macOS 14.8.9 the pattern matches but the target fails that check (panic "Failed to
// route createPspDirectory"). The only caller, `AmdBiosParserHelper::readPspFirmwareInfo`, is exported, so find the
// call there (whole kext as fallback) and decode it directly. The target must stay near the kext and start with the
// usual `push rbp; mov rbp, rsp` prologue.
static mach_vm_address_t solveCreatePspDirectoryBC250(KernelPatcher& patcher, const size_t id,
                                                     const mach_vm_address_t slide, const size_t size)
{
    static constexpr size_t kCallerScanSize = 0x100;
    static constexpr size_t kMaxDistance    = 0x4000000;
    static const UInt8      kPrologue[]     = {0x55, 0x48, 0x89, 0xE5};

    patcher.clearError();
    const auto caller =
        patcher.solveSymbol(id, "__ZN34AMDRadeonX6000_AmdBiosParserHelper19readPspFirmwareInfoEv", slide, size, true);
    patcher.clearError();
    const struct {
        mach_vm_address_t start;
        size_t            len;
    } ranges[] = {{caller, kCallerScanSize}, {slide, size}};

    for (const auto& range : ranges) {
        if (range.start == 0 || range.len == 0) { continue; }
        size_t off = 0;
        if (!KernelPatcher::findPattern(kCreatePspDirectoryCallPattern, kCreatePspDirectoryCallPatternMask,
                                        arrsize(kCreatePspDirectoryCallPattern),
                                        reinterpret_cast<const void*>(range.start), range.len, &off)) {
            BCLOG("X6000FB", "BC-250: createPspDirectory call not found in 0x%llX+0x%zX", range.start, range.len);
            continue;
        }
        const auto site = range.start + off + kCreatePspDirectoryCallPatternJumpInstOff;
        const auto rel  = *reinterpret_cast<const SInt32*>(site + 1);
        const auto target =
            static_cast<mach_vm_address_t>(static_cast<SInt64>(site + 5) + static_cast<SInt64>(rel));
        const auto distance = target > slide ? target - slide : slide - target;
        if (distance >= kMaxDistance) {
            BCLOG("X6000FB", "BC-250: createPspDirectory target is 0x%llX bytes from the kext, rejecting", distance);
            continue;
        }
        if (memcmp(reinterpret_cast<const void*>(target), kPrologue, sizeof(kPrologue)) != 0) {
            BCLOG("X6000FB", "BC-250: createPspDirectory target has no function prologue, rejecting");
            continue;
        }
        return target;
    }
    return 0;
}

void X6000FB::processKextCyanSkillfish(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
{

    NRed::singleton().hwLateInit();

    CAILAsicCapsEntry*                   orgAsicCapsTable       = nullptr;
    void*                                orgAmdAsicInfoNavi10VT = nullptr;
    PenguinWizardry::PatternSolveRequest solveRequests[]        = {
        {"__ZL20CAIL_ASIC_CAPS_TABLE", orgAsicCapsTable, kCailAsicCapsTablePattern},
        {"__ZN37AMDRadeonX6000_AmdDeviceMemoryManager17mapMemorySubRangeE25AmdReservedMemorySelectoryyj",
         this->mapMemorySubRange},
        {"__ZTV32AMDRadeonX6000_AmdAsicInfoNavi10", orgAmdAsicInfoNavi10VT},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper20readEfiAtomBiosImageEPhm", this->readEfiAtomBiosImage},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper20readPciAtomBiosImageEPhm", this->readPciAtomBiosImage},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper21validateAtomBiosImageEPhm", this->validateAtomBiosImage},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "X6000FB",
               "BC-250: Failed to resolve symbols");

    // Find a Navi entry to borrow DDI caps and the skeleton from. The table starts at the first (Tahiti) entry and is
    // long (hundreds of SI/CI/VI/... rows on macOS 14), so walk it to its terminator. scripts/bc250-preflight.py uses
    // the same walk, so it predicts this result without a reboot.
    const CAILAsicCapsEntry* naviEntry = nullptr;
    for (size_t i = 0; i < 8192; i += 1) {
        const auto& entry = orgAsicCapsTable[i];
        if (entry.familyId == 0 || entry.familyId > 0xFF || entry.deviceId == 0 || entry.deviceId == 0xFFFFFFFF ||
            entry.ddiCaps == nullptr)
        {
            DBGLOG("X6000FB", "BC-250: CAIL_ASIC_CAPS_TABLE ends after %zu entries", i);
            break;
        }
        if (entry.familyId == AMD_FAMILY_NAVI) {
            DBGLOG("X6000FB", "BC-250: Navi caps entry %zu: device 0x%X rev 0x%X ext 0x%X", i, entry.deviceId,
                   entry.revision, entry.extRevision);
            if (naviEntry == nullptr || entry.deviceId == 0x731F) { naviEntry = &entry; }
        }
    }
    PANIC_COND(naviEntry == nullptr, "X6000FB", "BC-250: No Navi entry in CAIL_ASIC_CAPS_TABLE");
    // Navi 10's caps as they are: Linux scans out from the carve-out (`sg_display=0`) on this chip, the dGPU-style
    // behaviour they describe.
    memcpy(bc250DdiCaps, naviEntry->ddiCaps, sizeof(bc250DdiCaps));

    PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X6000FB",
               "Failed to enable kernel writing");
    getMember<decltype(getGpuBrandingNameListBC250)*>(orgAmdAsicInfoNavi10VT, 0x228) = getGpuBrandingNameListBC250;
    const auto skeleton = naviEntry->skeleton;
    *orgAsicCapsTable   = {
          .familyId    = AMD_FAMILY_NAVI,
          .deviceId    = NRed::singleton().getDeviceID(),
          .revision    = NRed::singleton().getDevRevision(),
          .extRevision = static_cast<UInt32>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision(),
          .pciRevision = NRed::singleton().getPciRevision(),
          .ddiCaps     = bc250DdiCaps,
          .skeleton    = skeleton,
    };
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

    // Same macOS 13+ reverts as the iGPU path: there is no accelerator to wait for in framebuffer mode.
    if (currentKernelVersion() >= MACOS_13) {
        this->orgMessageAccelerator = patcher.solveSymbol<decltype(this->orgMessageAccelerator)>(
            id, "__ZNK34AMDRadeonX6000_AmdRadeonController18messageAcceleratorE25_eAMDAccelIOFBRequestTypePvS1_S1_",
            slide, size);
        PANIC_COND(this->orgMessageAccelerator == nullptr, "X6000FB", "Failed to resolve messageAccelerator");

        KernelPatcher::RouteRequest request{"__ZN34AMDRadeonX6000_AmdRadeonController7powerUpEv", wrapControllerPowerUp,
                                            this->orgControllerPowerUp};
        PANIC_COND(!patcher.routeMultiple(id, &request, 1, slide, size), "X6000FB", "Failed to route powerUp");

        const PenguinWizardry::MaskedLookupPatch patches[] = {
            {&kextRadeonX6000Framebuffer, kControllerPowerUpOriginal, kControllerPowerUpOriginalMask,
             kControllerPowerUpReplace, kControllerPowerUpReplaceMask, 1},
            {&kextRadeonX6000Framebuffer, kValidateDetailedTimingOriginal, kValidateDetailedTimingPatched, 1},
        };
        PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X6000FB",
                   "Failed to apply logic revert patches");
    }

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"__ZNK15AmdAtomVramInfo16populateVramInfoER16AtomFirmwareInfo", wrapPopulateVramInfo, kPopulateVramInfoPattern,
         kPopulateVramInfoPatternMask},
        {"__ZNK32AMDRadeonX6000_AmdAsicInfoNavi1027getEnumeratedRevisionNumberEv", getEnumeratedRevision},
        {"__ZN41AMDRadeonX6000_AmdDeviceMemoryManagerNavi21intializeReservedVramEv", initialiseReservedVRAM},
        {"__ZN38AMDRadeonX6000_AmdRadeonControllerNavi19setupBootWatermarksEv", dummyIOReturnSuccess},
        {"__ZNK30AMDRadeonX6000_AmdAgdcServices13getVendorInfoEP16AGDCVendorInfo_tm", wrapGetVendorInfo,
         this->orgGetVendorInfo},
        {"__ZN34AMDRadeonX6000_AmdBiosParserHelper12readAtomBiosEv", readAtomBios},
    };
    PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "X6000FB",
               "BC-250: Failed to route symbols");

    // These only take over when the VBIOS lacks the table (tableOffset == 0), so a VBIOS that has a real vram_info
    // or PSP directory keeps Apple's parser.
    PenguinWizardry::JumpPatternRouteRequest atombiosRequests[] = {
        {"__ZN15AmdAtomVramInfo14createVramInfoEP15AmdAtomFwHelperj", wrapCreateVramInfo, this->orgCreateVramInfo,
         kCreateVramInfoCallPattern, kCreateVramInfoCallPatternMask, kCreateVramInfoCallPatternJumpInstOff},
        {"__ZN17AmdAtomObjectInfo16createObjectInfoEP15AmdAtomFwHelperj", wrapCreateObjectInfo,
         this->orgCreateObjectInfo, kCreateObjectInfoCallPattern, kCreateObjectInfoCallPatternMask,
         kCreateObjectInfoCallPatternJumpInstOff},
    };
    PANIC_COND(!PenguinWizardry::JumpPatternRouteRequest::routeAll(patcher, id, atombiosRequests, slide, size),
               "X6000FB", "BC-250: Failed to route ATOMBIOS-related functions");

    if (currentKernelVersion() >= MACOS_11) {
        const auto pspTarget = solveCreatePspDirectoryBC250(patcher, id, slide, size);
        PANIC_COND(pspTarget == 0, "X6000FB", "BC-250: Failed to solve createPspDirectory");
        KernelPatcher::RouteRequest pspRequest{nullptr, wrapCreatePspDirectory, this->orgCreatePspDirectory};
        pspRequest.from = pspTarget;
        PANIC_COND(!patcher.routeMultiple(id, &pspRequest, 1), "X6000FB", "BC-250: Failed to route createPspDirectory");
        DBGLOG("X6000FB", "BC-250: routed createPspDirectory at 0x%llX", pspTarget);

        if (currentKernelVersion() <= MACOS_12_X) {
            PenguinWizardry::PatternRouteRequest request{
                "__ZN38AMDRadeonX6000_AmdRadeonControllerNavi21getTriageHardwareDataEjP12_AMD_TRIAGE_",
                getTriageHardwareDataStub};
            PANIC_COND(!request.route(patcher, id, slide, size), "X6000FB",
                       "BC-250: Failed to route getTriageHardwareData");
        }
    }

    // DCN 2.01: 4 pipes, so create only 4 cursors, links and underflow trackers (Navi 10 creates 6).
    auto* const orgCreateControllerServices = patcher.solveSymbol<void*>(
        id, "__ZN40AMDRadeonX6000_AmdRadeonControllerNavi1024createControllerServicesEv", slide, size, true);
    PANIC_COND(orgCreateControllerServices == nullptr, "X6000FB", "Failed to solve createControllerServices");
    auto* const orgSetupCursors =
        patcher.solveSymbol<void*>(id, "__ZN34AMDRadeonX6000_AmdRadeonController12setupCursorsEv", slide, size, true);
    PANIC_COND(orgSetupCursors == nullptr, "X6000FB", "Failed to solve setupCursors");
    auto* const orgCreateLinks =
        patcher.solveSymbol<void*>(id, "__ZN34AMDRadeonX6000_AmdRadeonController11createLinksEv", slide, size, true);
    PANIC_COND(orgCreateLinks == nullptr, "X6000FB", "Failed to solve createLinks");

    if (currentKernelVersion() <= MACOS_10_15_X) {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(
                       orgCreateControllerServices, PAGE_SIZE, kCreateControllerServicesOriginal1015,
                       kCreateControllerServicesOriginalMask1015, kCreateControllerServicesPatched1015,
                       kCreateControllerServicesPatchedMask1015, 1, 0),
                   "X6000FB", "Failed to apply createControllerServices patch (10.15)");
    }
    else {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(
                       orgCreateControllerServices, PAGE_SIZE, kCreateControllerServicesOriginal,
                       kCreateControllerServicesOriginalMask, kCreateControllerServicesPatched,
                       kCreateControllerServicesPatchedMask, 2, 0),
                   "X6000FB", "Failed to apply createControllerServices patch");
    }
    PANIC_COND(!KernelPatcher::findAndReplaceWithMask(
                   orgSetupCursors, PAGE_SIZE,
                   currentKernelVersion() >= MACOS_12 ? kSetupCursorsOriginal12 : kSetupCursorsOriginal,
                   currentKernelVersion() >= MACOS_12 ? kSetupCursorsOriginalMask12 : kSetupCursorsOriginalMask,
                   currentKernelVersion() >= MACOS_12 ? kSetupCursorsPatched12 : kSetupCursorsPatched,
                   currentKernelVersion() >= MACOS_12 ? kSetupCursorsPatchedMask12 : kSetupCursorsPatchedMask, 1, 0),
               "X6000FB", "Failed to apply setupCursors patch");
    PANIC_COND(!KernelPatcher::findAndReplaceWithMask(orgCreateLinks, PAGE_SIZE, kCreateLinksOriginal,
                                                      kCreateLinksOriginalMask, kCreateLinksPatched,
                                                      kCreateLinksPatchedMask, 1, 0),
               "X6000FB", "Failed to apply createLinks patch");

    // DCN 2.0.1 quirks: UMA scanout addresses, DENTIST VCO, 4 pipes. Soft-fails per quirk.
    BC250DCN::singleton().processKext(patcher, id, slide, size);

}
