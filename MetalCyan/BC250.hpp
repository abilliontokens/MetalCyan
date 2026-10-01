// ASRock BC-250 (AMD Cyan Skillfish) Support
//
// Copyright © 2026 NootedRed-BC250 contributors. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/IOTypes.h>
#include <libkern/c++/OSDictionary.h>

// Lilu's SYSLOG only reaches the kernel message buffer, which loses early-boot lines on macOS 14. BCLOG also keeps
// the line in a 128 KiB in-kext log, readable any time with `sysctl -n debug.bc250.log`.
void bc250LogAppend(const char* module, const char* format, ...) __attribute__((format(printf, 2, 3)));
#define BCLOG(module, str, ...)                                                                                        \
    do {                                                                                                               \
        SYSLOG(module, str, ##__VA_ARGS__);                                                                            \
        bc250LogAppend(module, str, ##__VA_ARGS__);                                                                    \
    } while (0)

class BC250
{
public:
    // What the probe learnt about the board. Zero means "unknown".
    struct Info
    {
        UInt32 carveOutMiB{0};       // RCC_CONFIG_MEMSIZE
        UInt64 bar0Size{0};          // CPU-visible VRAM aperture
        UInt64 fbLocationBase{0};    // GPU MC address of the carve-out
        UInt64 fbLocationTop{0};
        UInt32 gbAddrConfig{0};
        UInt32 activeCUs{0};         // From IP discovery GC info, if present
        UInt8  gcMajor{0}, gcMinor{0}, gcRevision{0};
        UInt8  dcnMajor{0}, dcnMinor{0}, dcnRevision{0};
        UInt8  sdmaCount{0};
        bool   hasVCN{false};
        bool   hasDiscovery{false};
        bool   hasVBIOS{false};
        bool   smuFirmwareRunning{false};
    };

    static BC250& singleton();

    const Info& getInfo() const { return this->info; }
    // The probe read the board: the AMD kexts are injected and patched for it. Otherwise nothing is, and macOS keeps
    // the firmware framebuffer.
    bool        isActive() const { return this->active; }

    // Called by NRed once the device has been identified as Cyan Skillfish.
    void processPatcher();

private:
    bool active{false};
    Info info{};

    void probe();
    void probeRegisters(OSDictionary* dict);
    void probeDiscovery(OSDictionary* dict);
    void parseDiscovery(const UInt8* bin, size_t size, OSDictionary* dict);
    void probeVBIOS(OSDictionary* dict);
};
