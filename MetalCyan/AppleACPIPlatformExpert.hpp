// Access to the protected ACPI table getter of the platform expert
//
// Copyright © 2022-2025 ChefKiss. Licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <IOKit/acpi/IOACPIPlatformExpert.h>

// Hack
class AppleACPIPlatformExpert : IOACPIPlatformExpert
{
    friend class X6000FB;
    friend class BC250;
};
