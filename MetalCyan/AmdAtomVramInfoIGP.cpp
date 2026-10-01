// AmdAtomVramInfo IGP implementation
//
// Copyright © 2026 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "AmdAtomVramInfoIGP.hpp"
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <GPUDriversAMD/FB/BiosParser.hpp>
#include <Headers/kern_util.hpp>
#include <NRed.hpp>

AmdAtomVramInfoIGP::AmdAtomVramInfoIGP(DataTableInitInfo* initInfo) :
    AmdAtomVramInfo(initInfo)
{ }

AmdAtomVramInfoIGP::~AmdAtomVramInfoIGP() { }

AmdAtomVramInfoIGP* AmdAtomVramInfoIGP::createVramInfoIGP(AmdAtomFwHelper* const biosHelper, const UInt32 tableOffset)
{
    BIOS_ASSERT(tableOffset != 0, nullptr);
    BIOS_ASSERT(biosHelper != nullptr, nullptr);

    DataTableInitInfo initInfo;
    initInfo.biosHelper  = biosHelper;
    initInfo.tableOffset = tableOffset;
    biosHelper->getAtomDataTableRevision(initInfo.tableOffset, initInfo.tableRevision);

    if (initInfo.tableRevision.major == 1 && initInfo.tableRevision.minor >= 11 && initInfo.tableRevision.minor <= 12) {
        return new AmdAtomVramInfoIGPV1(&initInfo);
    }

    if (initInfo.tableRevision.major == 2 && initInfo.tableRevision.minor >= 1 && initInfo.tableRevision.minor <= 3) {
        return new AmdAtomVramInfoIGPV2(&initInfo);
    }

    SYSLOG("X6000FB", "%s -- Unsupported version %d.%d", __PRETTY_FUNCTION__, initInfo.tableRevision.major,
           initInfo.tableRevision.minor);
    return nullptr;
}

UInt32 AmdAtomVramInfoIGP::translateMemoryTypeWidth(const ATOMDMIT17MemType memoryType)
{ return memoryType == kATOMDMIT17MemTypeLPDDR5 ? 32 : 64; }

ATOMVRAMType AmdAtomVramInfoIGP::translateMemoryType(const ATOMDMIT17MemType memoryType)
{
    switch (memoryType) {
        case kATOMDMIT17MemTypeHBM:
        case kATOMDMIT17MemTypeHBM2   : return kATOMVRAMTypeHBM2;
        case kATOMDMIT17MemTypeGDDR6  : return kATOMVRAMTypeGDDR6;
        case kATOMDMIT17MemTypeDDR    :
        case kATOMDMIT17MemTypeLPDDR  :
        case kATOMDMIT17MemTypeDDR2   :
        case kATOMDMIT17MemTypeLPDDR2 :
        case kATOMDMIT17MemTypeDDR3   :
        case kATOMDMIT17MemTypeLPDDR3 : return kATOMVRAMTypeDDR3;
        case kATOMDMIT17MemTypeDDR4   :
        case kATOMDMIT17MemTypeLPDDR4 :
        case kATOMDMIT17MemTypeDDR5   :
        case kATOMDMIT17MemTypeLPDDR5 :
        case kATOMDMIT17MemTypeLPDDR5x: return kATOMVRAMTypeDDR4;
        default                       : return kATOMVRAMTypeUnknown;
    }
}

void AmdAtomVramInfoIGP::fillVramInfo(VramInfo& info, const UInt8 channelCount, const ATOMDMIT17MemType memoryType)
{
    // BC-250: IntegratedSystemInfo v1.11 reports memory type 0x6 (not a real DMI type 17 memory type; Linux also
    // calls it UNKNOWN) and 16 channels. The board has 256-bit GDDR6: 16 channels x 16 bits.
    if (NRed::singleton().getAttributes().isCyanSkillfish() && translateMemoryType(memoryType) == kATOMVRAMTypeUnknown) {
        info.vramModuleCount            = 1;
        info.vramModules[0].memoryType  = kATOMVRAMTypeGDDR6;
        info.vramModules[0].numChannels = channelCount;
        info.vramModules[0].vramWidth   = static_cast<UInt32>(channelCount) * 16;
        return;
    }

    info.vramModuleCount            = 1;
    info.vramModules[0].memoryType  = translateMemoryType(memoryType);
    info.vramModules[0].numChannels = channelCount;
    info.vramModules[0].vramWidth   = static_cast<UInt32>(channelCount) * translateMemoryTypeWidth(memoryType);
}

AmdAtomVramInfoIGPV1::AmdAtomVramInfoIGPV1(DataTableInitInfo* initInfo) :
    AmdAtomVramInfoIGP(initInfo)
{
    m_infoTable.igpInfoV1 = initInfo->biosHelper->getImage<ATOMIGPSystemInfoV1>(m_tableOffset);
    BIOS_ASSERT_VALID(m_infoTable.igpInfoV1 != nullptr);
}

AmdAtomVramInfoIGPV1::~AmdAtomVramInfoIGPV1() { }

IOReturn AmdAtomVramInfoIGPV1::getVramInfo(VramInfo& info) const
{
    BIOS_ASSERT(m_infoTable.igpInfoV1 != nullptr, kIOReturnError);

    fillVramInfo(info, m_infoTable.igpInfoV1->umaChannelCount == 0 ? 1 : m_infoTable.igpInfoV1->umaChannelCount,
                 m_infoTable.igpInfoV1->memoryType);

    return kIOReturnSuccess;
}

AmdAtomVramInfoIGPV2::AmdAtomVramInfoIGPV2(DataTableInitInfo* initInfo) :
    AmdAtomVramInfoIGP(initInfo)
{
    m_infoTable.igpInfoV2 = initInfo->biosHelper->getImage<ATOMIGPSystemInfoV2>(m_tableOffset);
    BIOS_ASSERT_VALID(m_infoTable.igpInfoV2 != nullptr);
}

AmdAtomVramInfoIGPV2::~AmdAtomVramInfoIGPV2() { }

IOReturn AmdAtomVramInfoIGPV2::getVramInfo(VramInfo& info) const
{
    BIOS_ASSERT(m_infoTable.igpInfoV2 != nullptr, kIOReturnError);

    fillVramInfo(info, m_infoTable.igpInfoV2->umaChannelCount == 0 ? 1 : m_infoTable.igpInfoV2->umaChannelCount,
                 m_infoTable.igpInfoV2->memoryType);

    return kIOReturnSuccess;
}
