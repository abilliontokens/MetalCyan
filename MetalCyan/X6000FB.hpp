// AMDRadeonX6000Framebuffer Patches
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <AGDC/VendorInfo.hpp>
#include <GPUDriversAMD/FB/AmdAsicInfo.hpp>
#include <GPUDriversAMD/FB/AmdDeviceMemoryManager.hpp>
#include <GPUDriversAMD/FB/BiosParser.hpp>
#include <Headers/kern_patcher.hpp>

class X6000FB
{
    using IRQMGRWriteRegister_t = void(void* ctx, UInt64 off, UInt32 value);
    using IRQMGRReadRegister_t  = UInt32(void* ctx, UInt64 off);
    using mapMemorySubRange_t   = IOReturn(void* self, AmdReservedMemorySelector selector, size_t atOffset,
                                           size_t withSize, IOOptionBits andAttributes);
    using messageAccelerator_t  = IOReturn(void* self, UInt32 requestType, void* arg2, void* arg3, void* arg4);
    using readBiosImage_t       = size_t(const void* self, UInt8* buffer, size_t bufferSize);
    using validateBiosImage_t   = bool(const void* self, UInt8* buffer, size_t bufferSize);

    static constexpr UInt32 IOFBRequestControllerEnabled = 0x1B;

    mapMemorySubRange_t*   mapMemorySubRange{nullptr};
    mach_vm_address_t      orgDpReceiverPowerCtrl{0};
    messageAccelerator_t*  orgMessageAccelerator{nullptr};
    mach_vm_address_t      orgControllerPowerUp{0};
    mach_vm_address_t      orgCreateObjectInfo{0};
    mach_vm_address_t      orgCreateVramInfo{0};
    mach_vm_address_t      orgGetVendorInfo{0};
    readBiosImage_t*       readEfiAtomBiosImage{nullptr};
    readBiosImage_t*       readPciAtomBiosImage{nullptr};
    validateBiosImage_t*   validateAtomBiosImage{nullptr};
    mach_vm_address_t      orgCreatePspDirectory{0};

public:
    static X6000FB& singleton();

    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);

private:
    void processKextCyanSkillfish(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);
    static const AmdAsicBrandingTableEntry* getGpuBrandingNameListBC250(const void* self);
    static IOReturn                         getTriageHardwareDataStub(void* self, UInt32 fbIndex, void* triageData);

    static UInt16                           getEnumeratedRevision();
    static IOReturn                         wrapPopulateVramInfo(AmdAtomVramInfo* self, AtomFirmwareInfo& fwInfo);
    static IOReturn                         initialiseReservedVRAM(void* self);
    static IOReturn                         dummyIOReturnSuccess();
    static void                             wrapDpReceiverPowerCtrl(void* link, bool powerOn);
    static UInt32                           wrapControllerPowerUp(void* self);
    static void*                            wrapCreateObjectInfo(void* helper, UInt32 tableOffset);
    static AmdAtomVramInfo*                 wrapCreateVramInfo(AmdAtomFwHelper* biosHelper, UInt32 tableOffset);
    static IOReturn wrapGetVendorInfo(const void* self, AGDCVendorInfo_t* vendorInfo, size_t sizeofVendorInfo);
    static size_t   readVfctAtomBiosImage(void* self, UInt8* buffer, size_t bufferSize, bool strict = true);
    static size_t   readVramAtomBiosImage(void* self, UInt8* buffer, size_t bufferSize);
    static IOReturn readAtomBios(void* self);
    static AmdAtomPspDirectory* wrapCreatePspDirectory(AmdAtomFwHelper* biosHelper, UInt32 tableOffset);
};
