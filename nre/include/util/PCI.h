/*
 * Copyright (C) 2012, Nils Asmussen <nils@os.inf.tu-dresden.de>
 * Copyright (C) 2009-2010, Bernhard Kauer <bk@vmmon.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of NRE (NOVA runtime environment).
 *
 * NRE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NRE is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#pragma once

#include <kobj/Gsi.h>
#include <mem/DataSpace.h>
#include <services/ACPI.h>
#include <services/PCIConfig.h>
#include <Assert.h>

namespace nre {

class PCIException : public Exception {
public:
    DEFINE_EXCONSTRS(PCIException)
};

/**
 * A helper for PCI config space access.
 */
class PCI {
public:
    typedef PCIConfig::bdf_type bdf_type;
    typedef PCIConfig::value_type value_type;
    typedef uint cap_type;

private:
    static const value_type BAR0            = 4;
    static const value_type MAX_BAR     = 6;
    static const value_type BAR_TYPE_MASK   = 0x6;
    static const value_type BAR_TYPE_32B    = 0x0;
    static const value_type BAR_TYPE_64B    = 0x4;
    static const value_type BAR_IO          = 0x1;
    static const value_type BAR_IO_MASK = 0xFFFFFFFC;
    static const value_type BAR_MEM_MASK    = 0xFFFFFFF0;
    static const cap_type CAP_MSI           = 0x05U;
    static const cap_type CAP_MSIX          = 0x11U;
    static const cap_type CAP_PCIE          = 0x10U;

public:
    explicit PCI(PCIConfigSession &pcicfg, ACPISession *acpi = 0) : _pcicfg(pcicfg), _acpi(acpi) {
    }

    value_type conf_read(bdf_type bdf, size_t dword) {
        return _pcicfg.read(bdf, dword << 2);
    }
    void conf_write(bdf_type bdf, size_t dword, value_type value) {
        _pcicfg.write(bdf, dword << 2, value);
    }

    /**
     * Induce the number of the bars from the header-type.
     */
    uint count_bars(bdf_type bdf) {
        switch((conf_read(bdf, 0x3) >> 24) & 0x7f) {
            case 0:
                return 6;
            case 1:
                return 2;
            default:
                return 0;
        }
    }

    /**
     * Program the nr-th MSI/MSI-X vector of the given device.
     */
    Gsi *get_gsi_msi(bdf_type bdf, uint nr, void *msix_table = 0);

    /**
     * Returns the gsi and enables them.
     */
    Gsi *get_gsi(bdf_type bdf, uint nr, bool /*level*/ = false, void *msix_table = 0);

private:
    void init_msix_table(void *addr, bdf_type bdf, value_type msix_offset, uint nr, Gsi *gsi) {
        volatile uint *msix_table = reinterpret_cast<volatile uint*>(addr);
        msix_table[nr * 4 + 0] = gsi->msi_addr();
        msix_table[nr * 4 + 1] = gsi->msi_addr() >> 32;
        msix_table[nr * 4 + 2] = gsi->msi_value();
        msix_table[nr * 4 + 3] &= ~1;
        conf_write(bdf, msix_offset, 1U << 31);
    }

    /**
     * Find the position of a legacy PCI capability.
     */
    size_t find_cap(bdf_type bdf, cap_type id);

    /**
     * Find the position of an extended PCI capability.
     */
    size_t find_extended_cap(bdf_type bdf, cap_type id);

    /**
     * Get the base and the type of a bar.
     */
    uint64_t bar_base(bdf_type bdf, size_t bar, value_type *type = 0);

    /**
     * Determines BAR size. You should probably disable interrupt
     * delivery from this device, while querying BAR sizes.
     */
    uint64_t bar_size(bdf_type bdf, size_t bar, bool *is64bit = 0);

private:
    PCIConfigSession &_pcicfg;
    ACPISession *_acpi;
};

}
