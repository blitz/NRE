/*
 * Copyright (C) 2012, Nils Asmussen <nils@os.inf.tu-dresden.de>
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

#include <kobj/LocalThread.h>
#include <kobj/Pt.h>
#include <utcb/UtcbFrame.h>
#include <subsystem/ChildManager.h>
#include <subsystem/ChildHip.h>
#include <ipc/Service.h>
#include <util/Math.h>
#include <util/Cycler.h>
#include <String.h>
#include <Hip.h>
#include <CPU.h>
#include <Exception.h>
#include <Logging.h>
#include <cstring>
#include <new>
#include <Assert.h>

#include "PhysicalMemory.h"
#include "VirtualMemory.h"
#include "Hypervisor.h"
#include "Admission.h"
#include "SysInfoService.h"
#include "Log.h"

using namespace nre;

static const size_t MAX_CMDLINES_LEN    = ExecEnv::PAGE_SIZE;

class CPU0Init {
    CPU0Init();
    static CPU0Init init;
};

EXTERN_C void dlmalloc_init();
static void log_thread(void*);
static void sysinfo_thread(void*);
PORTAL static void portal_service(capsel_t);
PORTAL static void portal_pagefault(capsel_t);
PORTAL static void portal_startup(capsel_t pid);
static void start_childs();

CPU0Init CPU0Init::init INIT_PRIO_CPU0;

// stack for initial Thread (overwrites the weak-symbol defined in Startup.cc)
uchar _stack[ExecEnv::STACK_SIZE] ALIGNED(ARCH_STACK_SIZE);
// stacks for LocalThreads of first CPU
static uchar dsstack[ExecEnv::STACK_SIZE] ALIGNED(ARCH_STACK_SIZE);
static uchar ptstack[ExecEnv::STACK_SIZE] ALIGNED(ARCH_STACK_SIZE);
static uchar regptstack[ExecEnv::STACK_SIZE] ALIGNED(ARCH_STACK_SIZE);
static ChildManager *mng;

CPU0Init::CPU0Init() {
    // just init the current CPU to prevent that the startup-heap-size depends on the number of CPUs
    CPU &cpu = CPU::current();
    uintptr_t ds_utcb = VirtualMemory::alloc(Utcb::SIZE);
    // use the local stack here since we can't map dataspaces yet
    // note that we need a different thread here because otherwise we couldn't allocate any memory
    // on the heap in all portals that use the same thread (we would call us ourself).
    LocalThread *dsec = LocalThread::create(cpu.log_id(), ObjCap::INVALID,
                                            reinterpret_cast<uintptr_t>(dsstack), ds_utcb);
    cpu.ds_pt(new Pt(dsec, PhysicalMemory::portal_dataspace));
    // accept translated caps
    UtcbFrameRef dsuf(dsec->utcb());
    dsuf.accept_translates();

    uintptr_t ec_utcb = VirtualMemory::alloc(Utcb::SIZE);
    LocalThread *ec = LocalThread::create(cpu.log_id(), ObjCap::INVALID,
                                          reinterpret_cast<uintptr_t>(ptstack), ec_utcb);
    cpu.gsi_pt(new Pt(ec, Hypervisor::portal_gsi));
    cpu.io_pt(new Pt(ec, Hypervisor::portal_io));
    cpu.sc_pt(new Pt(ec, Admission::portal_sc));
    new Pt(ec, ec->event_base() + CapSelSpace::EV_STARTUP, portal_startup, Mtd(Mtd::RSP));
    new Pt(ec, ec->event_base() + CapSelSpace::EV_PAGEFAULT, portal_pagefault,
           Mtd(Mtd::RSP | Mtd::GPR_BSD | Mtd::RIP_LEN | Mtd::QUAL));
    // accept delegates and translates for portal_sc
    UtcbFrameRef defuf(ec->utcb());
    defuf.accept_delegates(0);
    defuf.accept_translates();

    // register portal for the log service
    uintptr_t regec_utcb = VirtualMemory::alloc(Utcb::SIZE);
    LocalThread *regec = LocalThread::create(cpu.log_id(), ObjCap::INVALID,
                                             reinterpret_cast<uintptr_t>(regptstack), regec_utcb);
    UtcbFrameRef reguf(regec->utcb());
    reguf.accept_delegates(CPU::order());
    cpu.srv_pt(new Pt(regec, portal_service));
}

int main() {
    {
        const Hip &hip = Hip::get();

        // add all available memory
        LOG(Logging::PLATFORM, Serial::get() << "Hip checksum is "
                                             << (hip.is_valid() ? "valid" : "not valid") << "\n");
        LOG(Logging::PLATFORM, Serial::get().writef("SEL: %u, EXC: %u, VMI: %u, GSI: %u\n",
                                                    hip.cfg_cap, hip.cfg_exc, hip.cfg_vm, hip.cfg_gsi));
        LOG(Logging::PLATFORM, Serial::get().writef("CPU runs @ %u Mhz, bus @ %u Mhz\n",
                                                    Hip::get().freq_tsc / 1000, Hip::get().freq_bus /
                                                    1000));
        for(Hip::mem_iterator it = hip.mem_begin(); it != hip.mem_end(); ++it) {
            // FIXME: why can't we use the memory above 4G?
            if(it->type == HipMem::AVAILABLE && it->addr < 0x100000000)
                PhysicalMemory::add(it->addr, it->size);
        }

        // remove all not available memory
        for(Hip::mem_iterator it = hip.mem_begin(); it != hip.mem_end(); ++it) {
            // also remove the BIOS-area (make it available as device-memory)
            if(it->type != HipMem::AVAILABLE || it->addr == 0)
                PhysicalMemory::remove(it->addr, Math::round_up<size_t>(it->size, ExecEnv::PAGE_SIZE));
            // make sure that we don't overwrite the next two pages behind the cmdline
            if(it->type == HipMem::MB_MODULE && it->aux)
                PhysicalMemory::remove(it->aux & ~(ExecEnv::PAGE_SIZE - 1), ExecEnv::PAGE_SIZE * 2);
        }

        // now allocate the available memory from the hypervisor
        PhysicalMemory::map_all();
        LOG(Logging::MEM_MAP, Serial::get() << "Virtual memory:\n" << VirtualMemory::regions());
        LOG(Logging::MEM_MAP, Serial::get() << "Physical memory:\n" << PhysicalMemory::regions());

        LOG(Logging::CPUS, Serial::get().writef("CPUs:\n"));
        for(CPU::iterator it = CPU::begin(); it != CPU::end(); ++it) {
            LOG(Logging::CPUS, Serial::get().writef("\tpackage=%u, core=%u, thread=%u, flags=%u\n",
                                                    it->package(), it->core(), it->thread(), it->flags()));
        }

        // now we can use dlmalloc (map-pt created and available memory added to pool)
        dlmalloc_init();

        // create memory mapping portals for the other CPUs
        Hypervisor::init();
        Admission::init();

        // now init the stuff for all other CPUs (using dlmalloc)
        for(CPU::iterator it = CPU::begin(); it != CPU::end(); ++it) {
            if(it->log_id() != CPU::current().log_id()) {
                // again, different thread for ds portal
                LocalThread *dsec = LocalThread::create(it->log_id());
                it->ds_pt(new Pt(dsec, PhysicalMemory::portal_dataspace));
                // accept translated caps
                UtcbFrameRef dsuf(dsec->utcb());
                dsuf.accept_translates();

                LocalThread *ec = LocalThread::create(it->log_id());
                it->gsi_pt(new Pt(ec, Hypervisor::portal_gsi));
                it->io_pt(new Pt(ec, Hypervisor::portal_io));
                it->sc_pt(new Pt(ec, Admission::portal_sc));
                // accept delegates and translates for portal_sc
                UtcbFrameRef defuf(ec->utcb());
                defuf.accept_delegates(0);
                defuf.accept_translates();
                new Pt(ec, ec->event_base() + CapSelSpace::EV_STARTUP, portal_startup, Mtd(Mtd::RSP));
                new Pt(ec, ec->event_base() + CapSelSpace::EV_PAGEFAULT, portal_pagefault,
                       Mtd(Mtd::RSP | Mtd::GPR_BSD | Mtd::RIP_LEN | Mtd::QUAL));

                // register portal for the log service
                LocalThread *regec = LocalThread::create(it->log_id());
                UtcbFrameRef reguf(ec->utcb());
                reguf.accept_delegates(CPU::order());
                it->srv_pt(new Pt(regec, portal_service));
            }
        }

        // change the Hip to allow us direct access to the mb-module-cmdlines
        char *cmdlines = new char[MAX_CMDLINES_LEN];
        char *curcmd = cmdlines;
        ChildHip *chip = new ChildHip(CPUSet(CPUSet::ALL));
        for(Hip::mem_iterator it = hip.mem_begin(); it != hip.mem_end(); ++it) {
            if(it->type != HipMem::MB_MODULE)
                chip->add_mem(it->addr, it->size, it->aux, it->type);
            else {
                char *cmdline = Hypervisor::map_string(it->aux);
                size_t len = strlen(cmdline) + 1;
                if(curcmd + len > cmdlines + MAX_CMDLINES_LEN)
                    Util::panic("Not enough memory for cmdlines");
                memcpy(curcmd, cmdline, len);
                chip->add_module(it->addr, it->size, curcmd);
                Hypervisor::unmap_string(cmdline);
                curcmd += len;
            }
        }
        chip->finalize();

        // exchange hip
        _startup_info.hip = chip;
    }

    const Hip &hip = Hip::get();
    LOG(Logging::MEM_MAP, Serial::get().writef("Memory map:\n"));
    for(Hip::mem_iterator it = hip.mem_begin(); it != hip.mem_end(); ++it) {
        LOG(Logging::MEM_MAP, Serial::get().writef("\taddr=%#Lx, size=%#Lx, type=%d, aux=%p",
                                                   it->addr, it->size, it->type, it->aux));
        if(it->aux)
            LOG(Logging::MEM_MAP, Serial::get().writef(" (%s)", it->cmdline()));
        LOG(Logging::MEM_MAP, Serial::get() << '\n');
    }

    mng = new ChildManager();
    GlobalThread::create(log_thread, CPU::current().log_id(), String("root-log"))->start();
    GlobalThread::create(sysinfo_thread, CPU::current().log_id(), String("root-sysinfo"))->start();

    // wait until log and sysinfo are registered
    while(mng->registry().find(String("log")) == 0 || mng->registry().find(String("sysinfo")) == 0)
        Util::pause();

    start_childs();

    Sm sm(0);
    sm.down();
    return 0;
}

static void log_thread(void*) {
    Log::get().start();
}

static void sysinfo_thread(void*) {
    SysInfoService *sysinfo = new SysInfoService(mng);
    sysinfo->start();
}

static void start_childs() {
    size_t mod = 0, i = 0;
    ForwardCycler<CPU::iterator> cpus(CPU::begin(), CPU::end());
    const Hip &hip = Hip::get();
    for(Hip::mem_iterator it = hip.mem_begin(); it != hip.mem_end(); ++it, ++mod) {
        // we are the first one :)
        if(it->type == HipMem::MB_MODULE && i++ >= 1) {
            // map the memory of the module
            uintptr_t virt = VirtualMemory::alloc(it->size);
            Hypervisor::map_mem(it->addr, virt, it->size);

            ChildConfig cfg(mod, String(it->cmdline()), cpus.next()->log_id());
            mng->load(virt, it->size, cfg);
            if(cfg.last())
                break;
        }
    }
}

static void portal_service(capsel_t) {
    UtcbFrameRef uf;
    try {
        Service::Command cmd;
        String name;
        uf >> cmd >> name;
        switch(cmd) {
            case Service::REGISTER: {
                BitField<Hip::MAX_CPUS> available;
                capsel_t cap = uf.get_delegated(uf.delegation_window().order()).offset();
                uf >> available;
                uf.finish_input();

                capsel_t sm = mng->reg_service(cap, name, available);
                uf.accept_delegates();
                uf.delegate(sm);
                uf << E_SUCCESS;
            }
            break;

            case Service::GET:
            case Service::UNREGISTER:
            case Service::CLIENT_DIED:
                uf.clear();
                uf << E_NOT_FOUND;
                break;
        }
    }
    catch(const Exception& e) {
        Syscalls::revoke(uf.delegation_window(), true);
        uf.clear();
        uf << e;
    }
}

static void portal_pagefault(capsel_t) {
    UtcbExcFrameRef uf;
    uintptr_t addrs[32];
    uintptr_t pfaddr = uf->qual[1];
    unsigned error = uf->qual[0];
    uintptr_t eip = uf->rip;

    Serial::get().writef("Root: Pagefault for %p @ %p on cpu %u, error=%#x\n",
                         pfaddr, eip, CPU::current().phys_id(), error);
    ExecEnv::collect_backtrace(uf->rsp & ~(ExecEnv::PAGE_SIZE - 1), uf->rbp, addrs, 32);
    Serial::get().writef("Backtrace:\n");
    for(uintptr_t *addr = addrs; *addr != 0; ++addr)
        Serial::get().writef("\t%p\n", *addr);

    // let the kernel kill us
    uf->rip = ExecEnv::KERNEL_START;
    uf->mtd = Mtd::RIP_LEN;
}

static void portal_startup(capsel_t) {
    UtcbExcFrameRef uf;
    uf->mtd = Mtd::RIP_LEN;
    uf->rip = *reinterpret_cast<word_t*>(uf->rsp + sizeof(word_t));
}
