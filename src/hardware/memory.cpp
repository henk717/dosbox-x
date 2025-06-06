/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <stdint.h>
#include <assert.h>
#include "dosbox.h"
#include "dos_inc.h"
#include "logging.h"
#include "mem.h"
#include "menu.h"
#include "inout.h"
#include "setup.h"
#include "paging.h"
#include "programs.h"
#include "zipfile.h"
#include "regs.h"
#include "bitop.h"
#ifndef WIN32
# include <stdlib.h>
# include <unistd.h>
# include <stdio.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <fcntl.h>
# if C_HAVE_MMAP
#  include <sys/mman.h>
# endif
#endif
#ifdef WIN32
# include <winioctl.h>
#endif

#include "voodoo.h"
#include "glidedef.h"

#include <string.h>

#if C_GAMELINK
#include "../gamelink/gamelink.h"
#endif // C_GAMELINK

void RebootLanguage(std::string filename, bool confirm=false);

/* memory from file, memory mapping */
#if C_HAVE_MMAP
# define DO_MEMORY_FILE
int			memory_file_fd = -1;
#elif defined(WIN32) && !defined(HX_DOS)
# define DO_MEMORY_FILE
# define WIN32_MMAP
HANDLE      memory_file_fd = INVALID_HANDLE_VALUE;
HANDLE      memory_file_map = INVALID_HANDLE_VALUE;
#endif

std::string		memory_file;
void*			memory_file_base = NULL;
size_t			memory_file_size = 0;
bool			memory_file_already_zero = false;

// TODO #ifdef WIN32 and not HX_DOS, a Win32 HANDLE to the memory file and a HANDLE to the memory map object.
// Memory mapping a file in Windows is completely different from Linux/Mac OS/etc.
//
// TODO: When above 4GB mapping is available, there will either be two memory mappings or the memory map will
//       just reflect physical memory layout, not yet decided. If we're able to do sparse files on most systems
//       the 64MB gap in memory will be empty file space that doesn't take up disk space.
//
// TODO: If we're going to allow the memory file as a means for cheat/hack programs to alter guest memory,
//       then there needs to be an API here to flush modified pages to disk. Or at least provide one so that
//       when you open the debugger, modified pages are flushed to disk for your analysis.

// ACPI memory region allocation.
// Most ACPI BIOSes actually use some region at top of memory, but the
// design of DOSBox-X doesn't make that possible, so the ACPI tables are
// written to a high memory region just below the top 4GB region and the
// RSD PTR in the legacy BIOS region (0xE0000-0xFFFFF) will point at that.
// A memory address is chosen, which must be maintained once tables are
// generated because tables point at each other by physical memory address.
// A fixed size region is chosen within which the tables are written.
//
// NTS: ACPI didn't happen until the Pentium era when it became quite rare
//      for CPUs to have less than 32 address bits. No 26-bit 486SX limits
//      here. For this reason, ACPI is not supported unless all 32 address
//      bits are enabled.
bool ACPI_enabled = false;
bool acpi_mem_setup = false;
uint32_t ACPI_BASE=0;
uint32_t ACPI_REGION_SIZE=0; // power of 2
uint32_t ACPI_version=0;
unsigned char *ACPI_buffer=NULL;
size_t ACPI_buffer_size=0;
int ACPI_IRQ=-1;
unsigned int ACPI_SMI_CMD=0;

class ACPIPageHandler : public PageHandler {
	public:
		ACPIPageHandler() : PageHandler(PFLAG_NOCODE|PFLAG_READABLE|PFLAG_WRITEABLE) {}
		ACPIPageHandler(Bitu flags) : PageHandler(flags) {}
		HostPt GetHostReadPt(PageNum phys_page) override {
			assert(ACPI_buffer != NULL);
			assert(ACPI_buffer_size >= 4096);
			phys_page -= (ACPI_BASE >> 12);
			phys_page &= (ACPI_REGION_SIZE >> 12) - 1;
			if (phys_page >= (ACPI_buffer_size >> 12)) phys_page = (ACPI_buffer_size >> 12) - 1;
			return ACPI_buffer + (phys_page << 12);
		}
		HostPt GetHostWritePt(PageNum phys_page) override {
			assert(ACPI_buffer != NULL);
			assert(ACPI_buffer_size >= 4096);
			phys_page -= (ACPI_BASE >> 12);
			phys_page &= (ACPI_REGION_SIZE >> 12) - 1;
			if (phys_page >= (ACPI_buffer_size >> 12)) phys_page = (ACPI_buffer_size >> 12) - 1;
			return ACPI_buffer + (phys_page << 12);
		}
};

static ACPIPageHandler acpi_mem_handler;

PageHandler* acpi_memio_cb(MEM_CalloutObject &co,Bitu phys_page) {
	(void)co;//UNUSED
	(void)phys_page;//UNUSED

	if (ACPI_buffer != NULL && ACPI_REGION_SIZE != 0 && phys_page >= (ACPI_BASE/4096) && phys_page < ((ACPI_BASE+ACPI_REGION_SIZE)/4096))
		return &acpi_mem_handler;

	return NULL;
}

void MEM_ResetPageHandler_Unmapped(Bitu phys_page, Bitu pages);

void ACPI_mem_enable(const bool enable) {
	if (enable && !acpi_mem_setup) {
		if (ACPI_BASE != 0 && ACPI_REGION_SIZE != 0) {
			MEM_SetPageHandler( ACPI_BASE/4096, ACPI_REGION_SIZE/4096, &acpi_mem_handler );
			acpi_mem_setup = true;
			PAGING_ClearTLB();
		}
	}
	else if (!enable && acpi_mem_setup) {
		if (ACPI_BASE != 0 && ACPI_REGION_SIZE != 0) {
			MEM_ResetPageHandler_Unmapped( ACPI_BASE/4096, ACPI_REGION_SIZE/4096 );
			acpi_mem_setup = false;
			PAGING_ClearTLB();
		}
	}
}

void ACPI_free() {
	if (ACPI_buffer != NULL) {
		delete[] ACPI_buffer;
		ACPI_buffer = NULL;
	}
	ACPI_buffer_size = 0;
}

bool ACPI_init() {
	if (ACPI_buffer == NULL) {
		if (ACPI_REGION_SIZE == 0 || ACPI_REGION_SIZE > (8ul << 20ull))
			return false;

		ACPI_buffer_size = ACPI_REGION_SIZE;
		ACPI_buffer = new unsigned char [ACPI_buffer_size];
		if (ACPI_buffer == NULL)
			return false;
	}

	return (ACPI_buffer != NULL);
}

static MEM_Callout_t lfb_mem_cb = MEM_Callout_t_none;
static MEM_Callout_t lfb_mmio_cb = MEM_Callout_t_none;

#define MEM_callouts_max (MEM_TYPE_MAX - MEM_TYPE_MIN)
#define MEM_callouts_index(t) (t - MEM_TYPE_MIN)

class MEM_callout_vector : public std::vector<MEM_CalloutObject> {
public:
    MEM_callout_vector() : std::vector<MEM_CalloutObject>() { };
public:
    unsigned int getcounter = 0;
    unsigned int alloc_from = 0;
};

static MEM_callout_vector MEM_callouts[MEM_callouts_max];

extern bool isa_memory_hole_15mb;

bool a20_guest_changeable = true;
bool a20_fake_changeable = false;
bool a20_fast_changeable = false;

bool enable_port92 = true;
bool has_Init_RAM = false;
bool has_Init_MemHandles = false;
bool has_Init_MemoryAccessArray = false;

extern Bitu rombios_minimum_location;
extern bool force_conversion;
extern bool VIDEO_BIOS_always_carry_14_high_font;
extern bool VIDEO_BIOS_always_carry_16_high_font;

static struct MemoryBlock {
    Bitu pages = 0;
    Bitu handler_pages = 0;
    Bitu reported_pages = 0;
    Bitu reported_pages_4gb = 0;
    PageHandler * * phandlers = NULL;
    MemHandle * mhandles = NULL;
    struct {
        Bitu        start_page;
        Bitu        end_page;
        Bitu        pages;
        PageHandler *handler;
    } lfb = {};
    struct {
        Bitu        start_page;
        Bitu        end_page;
        Bitu        pages;
        PageHandler *handler;
    } lfb_mmio = {};
    struct {
        bool enabled;
        uint8_t controlport;
    } a20 = {};
    uint32_t mem_alias_pagemask = 0;
    uint32_t mem_alias_pagemask_active = 0;
    uint32_t address_bits = 0;
    uint32_t hw_next_assign = 0;
} memory;

uint32_t MEM_get_address_bits() {
    return memory.address_bits;
}

uint32_t MEM_get_address_bits4GB() { /* some code cannot yet handle values larger than 32 */
    if (memory.address_bits > 32u)
        return 32u;
    else
        return memory.address_bits;
}

/* WARNING: When DOSBox-X enables emulation of more than 4GB of RAM, this MemBase and MemSize will only reflect the memory below 4GB.
 *          Which means phys_readx/writex(), which are limited to the first 4GB anyway (32-bit addresses), cannot be used to poke at
 *          memory above 4GB. Instead of extending MemBase and the singular allocation block to 4GB or larger, the memory above 4GB
 *          is a different block. The reason for this is that the gap that needs to be left open for PCI devices and the ROM BIOS is
 *          large enough that such an arrangement would lead to the waste of about 64MB of emulator memory, which is significant, while
 *          the 384KB wasted at the 8086 1MB limit is too small to worry about. */
HostPt MemBase = NULL;
size_t MemSize = 0;

class UnmappedPageHandler : public PageHandler {
public:
    UnmappedPageHandler() : PageHandler(PFLAG_INIT|PFLAG_NOCODE) {}
    uint8_t readb(PhysPt addr) override {
        (void)addr;//UNUSED
        return 0xFF; /* Real hardware returns 0xFF not 0x00 */
    } 
    void writeb(PhysPt addr,uint8_t val) override {
        (void)addr;//UNUSED
        (void)val;//UNUSED
    }
};

class IllegalPageHandler : public PageHandler {
public:
    IllegalPageHandler() : PageHandler(PFLAG_INIT|PFLAG_NOCODE) {}
    uint8_t readb(PhysPt addr) override {
        (void)addr;
#if C_DEBUG
        LOG_MSG("Warning: Illegal read from %lx (lin=%x), CS:IP %8x:%8x",(unsigned long)PAGING_GetPhysicalAddress64(addr),addr,SegValue(cs),reg_eip);
#else
        static Bits lcount=0;
        if (lcount<1000) {
            lcount++;
            //LOG_MSG("Warning: Illegal read from %x, CS:IP %8x:%8x",addr,SegValue(cs),reg_eip);
        }
#endif
        return 0xFF; /* Real hardware returns 0xFF not 0x00 */
    } 
    void writeb(PhysPt addr,uint8_t val) override {
        (void)addr;//UNUSED
        (void)val;//UNUSED
#if C_DEBUG
        LOG_MSG("Warning: Illegal write to %lx (lin=%x), CS:IP %8x:%8x",(unsigned long)PAGING_GetPhysicalAddress64(addr),addr,SegValue(cs),reg_eip);
#else
        static Bits lcount=0;
        if (lcount<1000) {
            lcount++;
            //LOG_MSG("Warning: Illegal write to %x, CS:IP %8x:%8x",addr,SegValue(cs),reg_eip);
        }
#endif
    }
};

class RAMPageHandler : public PageHandler {
public:
    RAMPageHandler() : PageHandler(PFLAG_READABLE|PFLAG_WRITEABLE) {}
    RAMPageHandler(Bitu flags) : PageHandler(flags) {}
    HostPt GetHostReadPt(PageNum phys_page) override {
        if (!a20_fast_changeable || (phys_page & (~0xFul/*64KB*/)) == 0x100ul/*@1MB*/)
            return MemBase+(phys_page&memory.mem_alias_pagemask_active)*MEM_PAGESIZE;

        return MemBase+phys_page*MEM_PAGESIZE;
    }
    HostPt GetHostWritePt(PageNum phys_page) override {
        if (!a20_fast_changeable || (phys_page & (~0xFul/*64KB*/)) == 0x100ul/*@1MB*/)
            return MemBase+(phys_page&memory.mem_alias_pagemask_active)*MEM_PAGESIZE;

        return MemBase+phys_page*MEM_PAGESIZE;
    }
};

class ROMAliasPageHandler : public PageHandler {
public:
    ROMAliasPageHandler() {
        flags=PFLAG_READABLE|PFLAG_HASROM;
    }
    HostPt GetHostReadPt(PageNum phys_page) override {
        return MemBase+((phys_page&0xF)+0xF0)*MEM_PAGESIZE;
    }
    HostPt GetHostWritePt(PageNum phys_page) override {
        return MemBase+((phys_page&0xF)+0xF0)*MEM_PAGESIZE;
    }
};

class ROMPageHandler : public RAMPageHandler {
public:
    ROMPageHandler() {
        flags=PFLAG_READABLE|PFLAG_HASROM;
    }
    void writeb(PhysPt addr,uint8_t val) override {
        if (IS_PC98_ARCH && (addr & ~0x7FFF) == 0xE0000u)
            { /* Many PC-98 games and programs will zero 0xE0000-0xE7FFF whether or not the 4th bitplane is mapped */ }
        else
            LOG(LOG_CPU,LOG_ERROR)("Write %x to rom at lin=%x phys=%llx",(int)val,(int)addr,(unsigned long long)PAGING_GetPhysicalAddress64(addr));
    }
    void writew(PhysPt addr,uint16_t val) override {
        if (IS_PC98_ARCH && (addr & ~0x7FFF) == 0xE0000u)
            { /* Many PC-98 games and programs will zero 0xE0000-0xE7FFF whether or not the 4th bitplane is mapped */ }
        else
            LOG(LOG_CPU,LOG_ERROR)("Write %x to rom at lin=%x phys=%llx",(int)val,(int)addr,(unsigned long long)PAGING_GetPhysicalAddress64(addr));
    }
    void writed(PhysPt addr,uint32_t val) override {
        if (IS_PC98_ARCH && (addr & ~0x7FFF) == 0xE0000u)
            { /* Many PC-98 games and programs will zero 0xE0000-0xE7FFF whether or not the 4th bitplane is mapped */ }
        else
            LOG(LOG_CPU,LOG_ERROR)("Write %x to rom at lin=%x phys=%llx",(int)val,(int)addr,(unsigned long long)PAGING_GetPhysicalAddress64(addr));
    }
};



static UnmappedPageHandler unmapped_page_handler;
static IllegalPageHandler illegal_page_handler;
static RAMPageHandler ram_page_handler;
static ROMPageHandler rom_page_handler;
static ROMAliasPageHandler rom_page_alias_handler;

PageHandler &Get_ROM_page_handler(void) {
    return rom_page_handler;
}

extern bool pcibus_enable;

template <enum MEM_Type_t iotype> static unsigned int MEM_Gen_Callout(Bitu &ret,PageHandler* &f,Bitu page) {
    int actual = iotype - MEM_TYPE_MIN;
    MEM_callout_vector &vec = MEM_callouts[actual];
    unsigned int match = 0;
    PageHandler *t_f;
    size_t scan = 0;

    (void)ret;//UNUSED

    while (scan < vec.size()) {
        MEM_CalloutObject &obj = vec[scan++];
        if (!obj.isInstalled()) continue;
        if (obj.m_handler == NULL) continue;
        if (!obj.MatchPage(page)) continue;

        t_f = obj.m_handler(obj,page);
        if (t_f != NULL) {
            if (match == 0) {
                f = t_f;
            }
            else {
                /* device conflict! */
                /* TODO: to handle it properly, we would need to know whether this was a memory read or memory write,
                 *       and then have some way for the slow mem page handler to call each page handler one by one
                 *       and either write to each or read from each and combine. */
                /* TODO: as usual, if iotype is PCI, multiple writes are permitted, but break out after finding one match for read. */
                break;
            }
            match++;
        }
    }

    return match;
}

static unsigned int MEM_Motherboard_Callout(Bitu &ret,PageHandler* &f,Bitu page) {
    return MEM_Gen_Callout<MEM_TYPE_MB>(ret,f,page);
}

static unsigned int MEM_PCI_Callout(Bitu &ret,PageHandler* &f,Bitu page) {
    return MEM_Gen_Callout<MEM_TYPE_PCI>(ret,f,page);
}

static unsigned int MEM_ISA_Callout(Bitu &ret,PageHandler* &f,Bitu page) {
    return MEM_Gen_Callout<MEM_TYPE_ISA>(ret,f,page);
}

static PageHandler *MEM_SlowPath(Bitu page) {
    PageHandler *f = &unmapped_page_handler;
    unsigned int match = 0;
    Bitu ret = ~0ul;

    if (page >= memory.handler_pages)
        return &illegal_page_handler;

    /* TEMPORARY, REMOVE LATER. SHOULD NOT HAPPEN. */
    if (page < memory.reported_pages) {
        if (page >= 0xf00 && page <= 0xfff && isa_memory_hole_15mb) { /* 0xF00000-0xFFFFFF (15MB-16MB) */
            /* ignore, ISA memory hole */
        }
        else {
            LOG(LOG_MISC,LOG_WARN)("MEM_SlowPath called within system RAM at page %x",(unsigned int)page);
            f = (PageHandler*)(&ram_page_handler);
        }
    }

    /* check motherboard devices (ROM BIOS, system RAM, etc.) */
    match = MEM_Motherboard_Callout(/*&*/ret,/*&*/f,page);

    if (match == 0) {
        /* first PCI bus device, then ISA. */
        if (pcibus_enable) {
            /* PCI and PCI/ISA bridge emulation */
            match = MEM_PCI_Callout(/*&*/ret,/*&*/f,page);
            if (match == 0) {
                /* PCI didn't take it, ask ISA bus */
                match = MEM_ISA_Callout(/*&*/ret,/*&*/f,page);
            }
        }
        else {
            /* Pure ISA emulation */
            match = MEM_ISA_Callout(/*&*/ret,/*&*/f,page);
        }
    }

    /* if nothing matched, assign default handler to MEM handler slot.
     * if one device responded, assign its handler to the MEM handler slot.
     * if more than one responded, then do not update the MEM handler slot. */
//    assert(iolen >= 1 && iolen <= 4);
//    porti = (iolen >= 4) ? 2 : (iolen - 1); /* 1 2 x 4 -> 0 1 1 2 */
    LOG(LOG_MISC,LOG_DEBUG)("MEM slow path page=%x: device matches=%u",(unsigned int)page,(unsigned int)match);
    if (match <= 1) memory.phandlers[page] = f;

    return f;
}

void MEM_RegisterHandler(Bitu phys_page,PageHandler * handler,Bitu page_range) {
    assert((phys_page+page_range) <= memory.handler_pages);
    while (page_range--) memory.phandlers[phys_page++]=handler;
}

void MEM_InvalidateCachedHandler(Bitu phys_page,Bitu range) {
    assert((phys_page+range) <= memory.handler_pages);
    while (range--) memory.phandlers[phys_page++]=NULL;
}

void MEM_FreeHandler(Bitu phys_page,Bitu page_range) {
    MEM_InvalidateCachedHandler(phys_page,page_range);
}

void MEM_CalloutObject::InvalidateCachedHandlers(void) {
    Bitu p;

    /* for both the base page, as well as its aliases, revert the pages back to "slow path" */
    for (p=m_base;p < memory.handler_pages;p += alias_mask+1)
        MEM_InvalidateCachedHandler(p,range_mask+1);
}

void MEM_CalloutObject::Install(Bitu page,Bitu pagemask/*MEMMASK_ISA_10BIT, etc.*/,MEM_CalloutHandler *handler) {
    if(!installed) {
        if (pagemask == 0 || (pagemask & ~0xFFFFFFFU)) {
            LOG(LOG_MISC,LOG_ERROR)("MEM_CalloutObject::Install: Page mask %x is invalid",(unsigned int)pagemask);
            return;
        }

        /* we need a mask for the distance between aliases of the port, and the range of I/O ports. */
        /* only the low part of the mask where bits are zero, not the upper.
         * This loop is the reason portmask cannot be ~0 else it would become an infinite loop.
         * This also serves to check that the mask is a proper combination of ISA masking and
         * I/O port range (example: IOMASK_ISA_10BIT & (~0x3) == 0x3FF & 0xFFFFFFFC = 0x3FC for a device with 4 I/O ports).
         * A proper mask has (from MSB to LSB):
         *   - zero or more 0 bits from MSB
         *   - 1 or more 1 bits in the middle
         *   - zero or more 0 bits to LSB */
        {
            Bitu m = 1;
            Bitu test;

            /* compute range mask from zero bits at LSB */
            range_mask = 0;
            test = pagemask ^ 0xFFFFFFFU;
            while ((test & m) == m) {
                range_mask = m;
                m = (m << 1) + 1;
            }

            /* DEBUG */
            if ((pagemask & range_mask) != 0 ||
                ((range_mask + 1) & range_mask) != 0/* should be a mask, therefore AND by itself + 1 should be zero (think (0xF + 1) & 0xF = 0x10 & 0xF = 0x0) */) {
                LOG(LOG_MISC,LOG_ERROR)("MEM_CalloutObject::Install: pagemask(%x) & range_mask(%x) != 0 (%x). You found a corner case that broke this code, fix it.",
                    (unsigned int)pagemask,
                    (unsigned int)range_mask,
                    (unsigned int)(pagemask & range_mask));
                return;
            }

            /* compute alias mask from middle 1 bits */
            alias_mask = range_mask;
            test = pagemask + range_mask; /* will break if portmask & range_mask != 0 */
            while ((test & m) == m) {
                alias_mask = m;
                m = (m << 1) + 1;
            }

            /* any bits after that should be zero. */
            /* confirm this by XORing portmask by (alias_mask ^ range_mask). */
            /* we already confirmed that portmask & range_mask == 0. */
            /* Example:
             *
             *    Sound Blaster at port 220-22Fh with 10-bit ISA decode would be 0x03F0 therefore:
             *      portmask =    0x03F0
             *      range_mask =  0x000F
             *      alias_mask =  0x03FF
             *
             *      portmask ^ range_mask = 0x3FF
             *      portmask ^ range_mask ^ alias_mask = 0x0000
             *
             * Example of invalid portmask 0x13F0:
             *      portmask =    0x13F0
             *      range_mask =  0x000F
             *      alias_mask =  0x03FF
             *
             *      portmask ^ range_mask = 0x13FF
             *      portmask ^ range_mask ^ alias_mask = 0x1000 */
            if ((pagemask ^ range_mask ^ alias_mask) != 0 ||
                ((alias_mask + 1) & alias_mask) != 0/* should be a mask, therefore AND by itself + 1 should be zero */) {
                LOG(LOG_MISC,LOG_ERROR)("MEM_CalloutObject::Install: pagemask(%x) ^ range_mask(%x) ^ alias_mask(%x) != 0 (%x). Invalid portmask.",
                    (unsigned int)pagemask,
                    (unsigned int)range_mask,
                    (unsigned int)alias_mask,
                    (unsigned int)(pagemask ^ range_mask ^ alias_mask));
                return;
            }

            if (page & range_mask) {
                LOG(LOG_MISC,LOG_ERROR)("MEM_CalloutObject::Install: page %x and page mask %x not aligned (range_mask %x)",
                    (unsigned int)page,(unsigned int)pagemask,(unsigned int)range_mask);
                return;
            }
        }

        installed=true;
        m_base=page;
        mem_mask=pagemask;
        m_handler=handler;

        /* add this object to the callout array.
         * don't register any I/O handlers. those will be registered during the "slow path"
         * callout process when the CPU goes to access them. to encourage that to happen,
         * we invalidate the I/O ranges */
        LOG(LOG_MISC,LOG_DEBUG)("MEM_CalloutObject::Install added device with page=0x%x mem_mask=0x%x rangemask=0x%x aliasmask=0x%x",
            (unsigned int)page,(unsigned int)mem_mask,(unsigned int)range_mask,(unsigned int)alias_mask);

        InvalidateCachedHandlers();
    }
}

void MEM_CalloutObject::Uninstall() {
    if(!installed) return;
    InvalidateCachedHandlers();
    installed=false;
}

/* callers maintain a handle to it.
 * if they need to touch it, they get a pointer, which they then have to put back.
 * The way DOSBox/DOSbox-X code handles MEM callbacks it's common to declare an MEM object,
 * call the install, but then never touch it again, so this should work fine.
 *
 * this allows us to maintain ready-made MEM callout objects to return quickly rather
 * than write more complicated code where the caller has to make an MEM_CalloutObject
 * and then call install and we have to add its pointer to a list/vector/whatever.
 * It also avoids problems where if we have to resize the vector, the pointers become
 * invalid, because callers have only handles and they have to put all the pointers
 * back in order for us to resize the vector. */
MEM_Callout_t MEM_AllocateCallout(MEM_Type_t t) {
    if (t < MEM_TYPE_MIN || t >= MEM_TYPE_MAX)
        return MEM_Callout_t_none;

    MEM_callout_vector &vec = MEM_callouts[t - MEM_TYPE_MIN];

try_again:
    while (vec.alloc_from < vec.size()) {
        MEM_CalloutObject &obj = vec[vec.alloc_from];

        if (!obj.alloc) {
            obj.alloc = true;
            assert(obj.isInstalled() == false);
            return MEM_Callout_t_comb(t,vec.alloc_from++); /* make combination, then increment alloc_from */
        }

        vec.alloc_from++;
    }

    /* okay, double the size of the vector within reason.
     * if anyone has pointers out to our elements, then we cannot resize. vector::resize() may invalidate them. */
    if (vec.size() < 4096 && vec.getcounter == 0) {
        size_t nsz = vec.size() * 2;

        LOG(LOG_MISC,LOG_WARN)("MEM_AllocateCallout type %u expanding array to %u",(unsigned int)t,(unsigned int)nsz);
        vec.alloc_from = (unsigned int)vec.size(); /* allocate from end of old vector size */
        vec.resize(nsz);
        goto try_again;
    }

    LOG(LOG_MISC,LOG_WARN)("MEM_AllocateCallout type %u no free entries",(unsigned int)t);
    return MEM_Callout_t_none;
}

void MEM_FreeCallout(MEM_Callout_t c) {
    enum MEM_Type_t t = MEM_Callout_t_type(c);

    if (t < MEM_TYPE_MIN || t >= MEM_TYPE_MAX)
        return;

    MEM_callout_vector &vec = MEM_callouts[t - MEM_TYPE_MIN];
    uint32_t idx = MEM_Callout_t_index(c);

    if (idx >= vec.size())
        return;

    MEM_CalloutObject &obj = vec[idx];
    if (!obj.alloc) return;

    if (obj.isInstalled())
        obj.Uninstall();

    obj.alloc = false;
    if (vec.alloc_from > idx)
        vec.alloc_from = idx; /* an empty slot just opened up, you can alloc from there */
}

MEM_CalloutObject *MEM_GetCallout(MEM_Callout_t c) {
    enum MEM_Type_t t = MEM_Callout_t_type(c);

    if (t < MEM_TYPE_MIN || t >= MEM_TYPE_MAX)
        return NULL;

    MEM_callout_vector &vec = MEM_callouts[t - MEM_TYPE_MIN];
    uint32_t idx = MEM_Callout_t_index(c);

    if (idx >= vec.size())
        return NULL;

    MEM_CalloutObject &obj = vec[idx];
    if (!obj.alloc) return NULL;
    obj.getcounter++;

    return &obj;
}

void MEM_PutCallout(MEM_CalloutObject *obj) {
    if (obj == NULL) return;
    if (obj->getcounter == 0) return;
    obj->getcounter--;
}

void lfb_mem_cb_free(void) {
    if (lfb_mem_cb != MEM_Callout_t_none) {
        MEM_FreeCallout(lfb_mem_cb);
        lfb_mem_cb = MEM_Callout_t_none;
    }
    if (lfb_mmio_cb != MEM_Callout_t_none) {
        MEM_FreeCallout(lfb_mmio_cb);
        lfb_mmio_cb = MEM_Callout_t_none;
    }
}

PageHandler* lfb_memio_cb(MEM_CalloutObject &co,Bitu phys_page) {
    (void)co;//UNUSED
    if (memory.lfb.start_page == 0 || memory.lfb.pages == 0)
        return NULL;
    if (phys_page >= memory.lfb.start_page && phys_page < memory.lfb.end_page)
        return memory.lfb.handler;
    if (phys_page >= memory.lfb_mmio.start_page && phys_page < memory.lfb_mmio.end_page)
        return memory.lfb_mmio.handler;

    return NULL;
}

void lfb_mem_cb_init() {
    if (lfb_mem_cb == MEM_Callout_t_none) {
        lfb_mem_cb = MEM_AllocateCallout(pcibus_enable ? MEM_TYPE_PCI : MEM_TYPE_ISA);
        if (lfb_mem_cb == MEM_Callout_t_none) E_Exit("Unable to allocate mem cb for LFB");
    }
    if (lfb_mmio_cb == MEM_Callout_t_none) {
        lfb_mmio_cb = MEM_AllocateCallout(pcibus_enable ? MEM_TYPE_PCI : MEM_TYPE_ISA);
        if (lfb_mmio_cb == MEM_Callout_t_none) E_Exit("Unable to allocate mmio cb for LFB");
    }

    {
        MEM_CalloutObject *cb = MEM_GetCallout(lfb_mem_cb);

        assert(cb != NULL);

        cb->Uninstall();

        if (memory.lfb.pages != 0) {
            Bitu p2sz = 1;
            /* make p2sz the largest power of 2 that covers the LFB */
            while (p2sz < memory.lfb.pages) p2sz <<= (Bitu)1;
            cb->Install(memory.lfb.start_page,MEMMASK_Combine(MEMMASK_FULL,MEMMASK_Range(p2sz)),lfb_memio_cb);
        }

        MEM_PutCallout(cb);
    }

    {
        MEM_CalloutObject *cb = MEM_GetCallout(lfb_mmio_cb);

        assert(cb != NULL);

        cb->Uninstall();
        if (memory.lfb_mmio.pages != 0) {
            Bitu p2sz = 1;
            /* make p2sz the largest power of 2 that covers the LFB */
            while (p2sz < memory.lfb_mmio.pages) p2sz <<= (Bitu)1;
            cb->Install(memory.lfb_mmio.start_page,MEMMASK_Combine(MEMMASK_FULL,MEMMASK_Range(p2sz)),lfb_memio_cb);
        }

        MEM_PutCallout(cb);
    }
}

/* TODO: At some point, this common code needs to be removed and the S3 emulation (or whatever else)
 *       needs to provide LFB and/or MMIO mapping. */
void MEM_SetLFB(Bitu page, Bitu pages, PageHandler *handler, PageHandler *mmiohandler) {
    if (page == memory.lfb.start_page && memory.lfb.end_page == (page+pages) &&
        memory.lfb.pages == pages && memory.lfb.handler == handler &&
        memory.lfb_mmio.handler == mmiohandler)
        return;

    memory.lfb.handler=handler;
    if (handler != NULL) {
        memory.lfb.start_page=page;
        memory.lfb.end_page=page+pages;
        memory.lfb.pages=pages;
    }
    else {
        memory.lfb.start_page=0;
        memory.lfb.end_page=0;
        memory.lfb.pages=0;
    }

    memory.lfb_mmio.handler=mmiohandler;
    if (mmiohandler != NULL) {
        /* FIXME: Why is this hard-coded? There's more than just S3 emulation in this code's future! */
        if (svgaCard == SVGA_S3Trio && (s3Card == S3_Trio64V || s3Card == S3_Vision868 || s3Card == S3_Vision968 || s3Card == S3_ViRGE || s3Card == S3_ViRGEVX)) {
            /* 64MB BAR. According to the datasheet, this 64MB region is split into two 32MB halves,
             * the lower half "little endian" and the upper half "big endian". Within the 32MB region,
             * the low 16MB is video memory and the high 16MB is MMIO. */
            memory.lfb_mmio.start_page=page+(0x01000000/4096);
            memory.lfb_mmio.end_page=page+(0x01000000/4096)+16;
            memory.lfb_mmio.pages=16;
        }
        else {
            /* 8MB BAR, MMIO at +16MB */
            memory.lfb_mmio.start_page=page+(0x01000000/4096);
            memory.lfb_mmio.end_page=page+(0x01000000/4096)+16;
            memory.lfb_mmio.pages=16;
        }
    }
    else {
        memory.lfb_mmio.start_page=0;
        memory.lfb_mmio.end_page=0;
        memory.lfb_mmio.pages=0;
    }

    if (pages == 0 || page == 0) {
        lfb_mem_cb_free();
        LOG(LOG_MISC,LOG_DEBUG)("MEM: Linear framebuffer disabled");
    }
    else {
        lfb_mem_cb_init();

        LOG(LOG_MISC,LOG_DEBUG)("MEM: Linear framebuffer is now set to 0x%lx-0x%lx (%uKB)",
            (unsigned long)(page*4096),
            (unsigned long)(((page+pages)*4096)-1),
            (unsigned int)(pages*4));
        // FIXME: Because this code emulates S3 by hardcoding the MMIO address!
        LOG(LOG_MISC,LOG_DEBUG)("MEM: Linear framebuffer MMIO is now set to 0x%lx-0x%lx (%uKB)",
            (unsigned long)(page*4096)+0x01000000,
            (unsigned long)(((page+16)*4096)+0x01000000-1),
            (unsigned int)(16*4));
    }

    PAGING_ClearTLB();
}

class Mem4GBPageHandler : public PageHandler {
	public:
		Mem4GBPageHandler() : PageHandler(PFLAG_READABLE|PFLAG_WRITEABLE) {}
		Mem4GBPageHandler(Bitu flags) : PageHandler(flags) {}
		HostPt GetHostReadPt(PageNum phys_page) override {
			assert(memory_file_base != NULL);
			const size_t ofs = size_t(phys_page) * size_t(4096u);
			assert(ofs < memory_file_size);
			return (unsigned char*)memory_file_base + ofs;
		}
		HostPt GetHostWritePt(PageNum phys_page) override {
			assert(memory_file_base != NULL);
			const size_t ofs = size_t(phys_page) * size_t(4096u);
			assert(ofs < memory_file_size);
			return (unsigned char*)memory_file_base + ofs;
		}
};

static Mem4GBPageHandler mem4gb_handler;

PageHandler * MEM_GetPageHandler(Bitu phys_page) {
	phys_page &= memory.mem_alias_pagemask_active;
	if (glide.enabled && (phys_page>=(GLIDE_LFB>>12)) && (phys_page<(GLIDE_LFB>>12)+GLIDE_PAGES))
		return (PageHandler*)glide.lfb_pagehandler;
	else if (phys_page<memory.handler_pages) {
		if (memory.phandlers[phys_page] != NULL) /*likely*/
			return memory.phandlers[phys_page];

		return MEM_SlowPath(phys_page); /* will also fill in phandlers[] if zero or one matches, so the next access is very fast */
	}

	if (phys_page >= 0x100000ul && phys_page < (0x100000ul+(unsigned long)memory.reported_pages_4gb)) {
		assert(memory_file_base != NULL);
		return &mem4gb_handler;
	}

	return &illegal_page_handler;
}

void MEM_SetPageHandler(Bitu phys_page,Bitu pages,PageHandler * handler) {
    for (;pages>0;pages--) {
        memory.phandlers[phys_page]=handler;
        phys_page++;
    }
}

void MEM_ResetPageHandler_RAM(Bitu phys_page, Bitu pages) {
    PageHandler *ram_ptr = (PageHandler*)(&ram_page_handler);
    for (;pages>0;pages--) {
        memory.phandlers[phys_page]=ram_ptr;
        phys_page++;
    }
}

void MEM_ResetPageHandler_Unmapped(Bitu phys_page, Bitu pages) {
    for (;pages>0;pages--) {
        memory.phandlers[phys_page]=&unmapped_page_handler;
        phys_page++;
    }
}

Bitu mem_strlen(LinearPt pt) {
    uint16_t x=0;
    while (x<1024) {
        if (!mem_readb_inline(pt+x)) return x;
        x++;
    }
    return 0;       //Hope this doesn't happen
}

void mem_strcpy(LinearPt dest,LinearPt src) {
    uint8_t r;
    while ( (r = mem_readb(src++)) ) mem_writeb_inline(dest++,r);
    mem_writeb_inline(dest,0);
}

void mem_memcpy(LinearPt dest,LinearPt src,Bitu size) {
    while (size--) mem_writeb_inline(dest++,mem_readb_inline(src++));
}

void MEM_BlockRead(LinearPt pt,void * data,Bitu size) {
    uint8_t * write=reinterpret_cast<uint8_t *>(data);
    while (size--) {
        *write++=mem_readb_inline(pt++);
    }
}

void MEM_BlockWrite(LinearPt pt, const void *data, size_t size) {
    const uint8_t* read = static_cast<const uint8_t *>(data);
    if (size==0)
        return;

    if ((pt >> 12) == ((pt+size-1)>>12)) { // Always same TLB entry
        HostPt tlb_addr=get_tlb_write(pt);
        if (!tlb_addr) {
            uint8_t val = *read++;
            get_tlb_writehandler(pt)->writeb(pt,val);
            tlb_addr=get_tlb_write(pt);
            pt++; size--;
            if (!tlb_addr) {
                // Slow path
                while (size--) {
                    mem_writeb_inline(pt++,*read++);
                }
                return;
            }
        }
        // Fast path
        memcpy(tlb_addr+pt, read, size);
    }
    else {
        const Bitu current = (((pt>>12)+1)<<12) - pt;
        Bitu remainder = size - current;
        MEM_BlockWrite(pt, data, current);
        MEM_BlockWrite((LinearPt)(pt + current), reinterpret_cast<uint8_t const*>(data) + current, remainder);
    }
}

void MEM_BlockRead32(LinearPt pt,void * data,Bitu size) {
    uint32_t * write=(uint32_t *) data;
    size>>=2;
    while (size--) {
        *write++=mem_readd_inline(pt);
        pt+=4;
    }
}

void MEM_BlockWrite32(LinearPt pt,void * data,Bitu size) {
    uint32_t * read=(uint32_t *) data;
    size>>=2;
    while (size--) {
        mem_writed_inline(pt,*read++);
        pt+=4;
    }
}

void MEM_BlockCopy(LinearPt dest,LinearPt src,Bitu size) {
    mem_memcpy(dest,src,size);
}

void MEM_StrCopy(LinearPt pt,char * data,Bitu size) {
    while (size--) {
        uint8_t r=mem_readb_inline(pt++);
        if (!r) break;
        *data++=(char)r;
    }
    *data=0;
}

Bitu MEM_TotalPages(void) {
    return memory.reported_pages;
}

Bitu MEM_TotalPagesAt4GB(void) {
    return memory.reported_pages_4gb;
}

Bitu MEM_FreeLargest(void) {
    Bitu size=0;Bitu largest=0;
    Bitu index=XMS_START;   
    while (index<memory.reported_pages) {
        if (!memory.mhandles[index]) {
            size++;
        } else {
            if (size>largest) largest=size;
            size=0;
        }
        index++;
    }
    if (size>largest) largest=size;
    return largest;
}

Bitu MEM_FreeTotal(void) {
    Bitu free=0;
    Bitu index=XMS_START;   
    while (index<memory.reported_pages) {
        if (!memory.mhandles[index]) free++;
        index++;
    }
    return free;
}

Bitu MEM_AllocatedPages(MemHandle handle) 
{
    Bitu pages = 0;
    while (handle>0) {
        pages++;
        handle=memory.mhandles[handle];
    }
    return pages;
}

//TODO Maybe some protection for this whole allocation scheme

INLINE uint32_t BestMatch(Bitu size) {
    uint32_t index=XMS_START;   
    uint32_t first=0;
    uint32_t best=0xfffffff;
    uint32_t best_first=0;
    while (index<memory.reported_pages) {
        /* Check if we are searching for first free page */
        if (!first) {
            /* Check if this is a free page */
            if (!memory.mhandles[index]) {
                first=index;    
            }
        } else {
            /* Check if this still is used page */
            if (memory.mhandles[index]) {
                uint32_t pages=index-first;
                if (pages==size) {
                    return first;
                } else if (pages>size) {
                    if (pages<best) {
                        best=pages;
                        best_first=first;
                    }
                }
                first=0;            //Always reset for new search
            }
        }
        index++;
    }
    /* Check for the final block if we can */
    if (first && (index-first>=size) && (index-first<best)) {
        return first;
    }
    return best_first;
}

/* alternate copy, that will only allocate memory on addresses
 * where the 20th address bit is zero. memory allocated in this
 * way will always be accessible no matter the state of the A20 gate */
INLINE uint32_t BestMatch_A20_friendly(Bitu size) {
    uint32_t index=XMS_START;
    uint32_t first=0;
    uint32_t best=0xfffffff;
    uint32_t best_first=0;

    /* if the memory to allocate is more than 1MB this function will never work. give up now. */
    if (size > 0x100)
        return 0;

    /*  TODO: For EMS allocation this would put things in the middle of extended memory space,
     *        which would increase possible memory fragmentation! Could we avoid memory fragmentation
     *        by instead scanning from the top down i.e. so the EMS system memory takes the top of
     *        extended memory and the DOS program is free to gobble up a large continuous range from
     *        below? */
    while (index<memory.reported_pages) {
        /* Check if we are searching for first free page */
        if (!first) {
            /* if the index is now on an odd megabyte, skip forward and try again */
            if (index & 0x100) {
                index = (index|0xFF)+1; /* round up to an even megabyte */
                continue;
            }

            /* Check if this is a free page */
            if (!memory.mhandles[index]) {
                first=index;
            }
        } else {
            /* Check if this still is used page or on odd megabyte */
            if (memory.mhandles[index] || (index & 0x100)) {
                uint32_t pages=index-first;
                if (pages==size) {
                    return first;
                } else if (pages>size) {
                    if (pages<best) {
                        best=pages;
                        best_first=first;
                    }
                }
                first=0;            //Always reset for new search
            }
        }
        index++;
    }
    /* Check for the final block if we can */
    if (first && (index-first>=size) && (index-first<best)) {
        return first;
    }
    return best_first;
}

MemHandle MEM_AllocatePages(Bitu pages,bool sequence) {
    MemHandle ret;
    if (!pages) return 0;
    if (sequence) {
        uint32_t index=BestMatch(pages);
        if (!index) return 0;
        MemHandle * next=&ret;
        while (pages) {
            *next=(MemHandle)index;
            next=&memory.mhandles[index];
            index++;pages--;
        }
        *next=-1;
    } else {
        if (MEM_FreeTotal()<pages) return 0;
        MemHandle * next=&ret;
        while (pages) {
            uint32_t index=BestMatch(1);
            if (!index) E_Exit("MEM:corruption during allocate");
            while (pages && (!memory.mhandles[index])) {
                *next=(MemHandle)index;
                next=&memory.mhandles[index];
                index++;pages--;
            }
            *next=-1;       //Invalidate it in case we need another match
        }
    }
    return ret;
}

/* alternate version guaranteed to allocate memory that is fully accessible
 * regardless of the state of the A20 gate. the physical memory address will
 * always have the 20th bit zero */
MemHandle MEM_AllocatePages_A20_friendly(Bitu pages,bool sequence) {
    MemHandle ret;
    if (!pages) return 0;
    if (sequence) {
        uint32_t index=BestMatch_A20_friendly(pages);
        if (!index) return 0;
#if C_DEBUG
        if (index & 0x100) E_Exit("MEM_AllocatePages_A20_friendly failed to make sure address has bit 20 == 0");
        if ((index+pages-1) & 0x100) E_Exit("MEM_AllocatePages_A20_friendly failed to make sure last page has bit 20 == 0");
#endif
        MemHandle * next=&ret;
        while (pages) {
            *next=(MemHandle)index;
            next=&memory.mhandles[index];
            index++;pages--;
        }
        *next=-1;
    } else {
        if (MEM_FreeTotal()<pages) return 0;
        MemHandle * next=&ret;
        while (pages) {
            uint32_t index=BestMatch_A20_friendly(1);
            if (!index) E_Exit("MEM:corruption during allocate");
#if C_DEBUG
            if (index & 0x100) E_Exit("MEM_AllocatePages_A20_friendly failed to make sure address has bit 20 == 0");
#endif
            while (pages && (!memory.mhandles[index])) {
                *next=(MemHandle)index;
                next=&memory.mhandles[index];
                index++;pages--;
            }
            *next=-1;       //Invalidate it in case we need another match
        }
    }
    return ret;
}

MemHandle MEM_GetNextFreePage(void) {
    return (MemHandle)BestMatch(1);
}

void MEM_ReleasePages(MemHandle handle) {
    if (memory.mhandles == NULL) {
        LOG(LOG_MISC,LOG_WARN)("MEM_ReleasePages() called when mhandles==NULL, nothing to release");
        return;
    }

    while (handle>0) {
        MemHandle next=memory.mhandles[handle];
        memory.mhandles[handle]=0;
        handle=next;
    }
}

bool MEM_ReAllocatePages(MemHandle & handle,Bitu pages,bool sequence) {
    if (handle<=0) {
        if (!pages) return true;
        handle=MEM_AllocatePages(pages,sequence);
        return (handle>0);
    }
    if (!pages) {
        MEM_ReleasePages(handle);
        handle=-1;
        return true;
    }
    MemHandle index=handle;
    MemHandle last;Bitu old_pages=0;
    while (index>0) {
        old_pages++;
        last=index;
        index=memory.mhandles[index];
    }
    if (old_pages == pages) return true;
    if (old_pages > pages) {
        /* Decrease size */
        pages--;index=handle;old_pages--;
        while (pages) {
            index=memory.mhandles[index];
            pages--;old_pages--;
        }
        MemHandle next=memory.mhandles[index];
        memory.mhandles[index]=-1;
        index=next;
        while (old_pages) {
            next=memory.mhandles[index];
            memory.mhandles[index]=0;
            index=next;
            old_pages--;
        }
        return true;
    } else {
        /* Increase size, check for enough free space */
        Bitu need=pages-old_pages;
        if (sequence) {
            index=last+1;
            Bitu free=0;
            while ((index<(MemHandle)memory.reported_pages) && !memory.mhandles[index]) {
                index++;free++;
            }
            if (free>=need) {
                /* Enough space allocate more pages */
                index=last;
                while (need) {
                    memory.mhandles[index]=index+1;
                    need--;index++;
                }
                memory.mhandles[index]=-1;
                return true;
            } else {
                /* Not Enough space allocate new block and copy */
                MemHandle newhandle=MEM_AllocatePages(pages,true);
                if (!newhandle) return false;
                MEM_BlockCopy((unsigned int)newhandle*4096u,(unsigned int)handle*4096u,(unsigned int)old_pages*4096u);
                MEM_ReleasePages(handle);
                handle=newhandle;
                return true;
            }
        } else {
            MemHandle rem=MEM_AllocatePages(need,false);
            if (!rem) return false;
            memory.mhandles[last]=rem;
            return true;
        }
    }
    return 0;
}

MemHandle MEM_NextHandle(MemHandle handle) {
    return memory.mhandles[handle];
}

MemHandle MEM_NextHandleAt(MemHandle handle,Bitu where) {
    while (where) {
        where--;    
        handle=memory.mhandles[handle];
    }
    return handle;
}


/* 
    A20 line handling, 
    Basically maps the 4 pages at the 1mb to 0mb in the default page directory
*/
bool MEM_A20_Enabled(void) {
    return memory.a20.enabled;
}

void MEM_A20_Enable(bool enabled) {
    if (memory.a20.enabled != enabled)
        LOG(LOG_MISC,LOG_DEBUG)("MEM_A20_Enable(%u)",enabled?1:0);

    if (a20_guest_changeable || a20_fake_changeable) {
        memory.a20.enabled = enabled;
        force_conversion = true;
        mainMenu.get_item("enable_a20gate").check(enabled).refresh_item(mainMenu);
        force_conversion = false;
    }

    if (!a20_fake_changeable && (memory.mem_alias_pagemask & 0x100ul)) {
        if (memory.a20.enabled) memory.mem_alias_pagemask_active |= 0x100ul;
        else memory.mem_alias_pagemask_active &= ~0x100ul;
        PAGING_ClearTLB();
    }
}


/* Memory access functions */
uint16_t mem_unalignedreadw(LinearPt address) {
    uint16_t ret = (uint16_t)mem_readb_inline(address);
    ret       |= (uint16_t)mem_readb_inline(address+1u) << 8u;
    return ret;
}

uint32_t mem_unalignedreadd(LinearPt address) {
    uint32_t ret = (uint32_t)mem_readb_inline(address   );
    ret       |= (uint32_t)mem_readb_inline(address+1u) << 8u;
    ret       |= (uint32_t)mem_readb_inline(address+2u) << 16u;
    ret       |= (uint32_t)mem_readb_inline(address+3u) << 24u;
    return ret;
}


void mem_unalignedwritew(LinearPt address,uint16_t val) {
    mem_writeb_inline(address,   (uint8_t)val);val>>=8u;
    mem_writeb_inline(address+1u,(uint8_t)val);
}

void mem_unalignedwrited(LinearPt address,uint32_t val) {
    mem_writeb_inline(address,   (uint8_t)val);val>>=8u;
    mem_writeb_inline(address+1u,(uint8_t)val);val>>=8u;
    mem_writeb_inline(address+2u,(uint8_t)val);val>>=8u;
    mem_writeb_inline(address+3u,(uint8_t)val);
}


bool mem_unalignedreadw_checked(LinearPt address, uint16_t * val) {
    uint8_t rval1,rval2;
    if (mem_readb_checked(address+0, &rval1)) return true;
    if (mem_readb_checked(address+1, &rval2)) return true;
    *val=(uint16_t)(rval1 | (rval2 << 8));
    return false;
}

bool mem_unalignedreadd_checked(LinearPt address, uint32_t * val) {
    uint8_t rval1,rval2,rval3,rval4;
    if (mem_readb_checked(address+0, &rval1)) return true;
    if (mem_readb_checked(address+1, &rval2)) return true;
    if (mem_readb_checked(address+2, &rval3)) return true;
    if (mem_readb_checked(address+3, &rval4)) return true;
    *val=(uint32_t)(rval1 | (rval2 << 8) | (rval3 << 16) | (rval4 << 24));
    return false;
}

bool mem_unalignedwritew_checked(LinearPt address,uint16_t val) {
    if (mem_writeb_checked(address,(uint8_t)(val & 0xff))) return true;
    val>>=8;
    if (mem_writeb_checked(address+1,(uint8_t)(val & 0xff))) return true;
    return false;
}

bool mem_unalignedwrited_checked(LinearPt address,uint32_t val) {
    if (mem_writeb_checked(address,(uint8_t)(val & 0xff))) return true;
    val>>=8;
    if (mem_writeb_checked(address+1,(uint8_t)(val & 0xff))) return true;
    val>>=8;
    if (mem_writeb_checked(address+2,(uint8_t)(val & 0xff))) return true;
    val>>=8;
    if (mem_writeb_checked(address+3,(uint8_t)(val & 0xff))) return true;
    return false;
}

uint8_t mem_readb(const LinearPt address) {
    return mem_readb_inline(address);
}

uint16_t mem_readw(const LinearPt address) {
    return mem_readw_inline(address);
}

uint32_t mem_readd(const LinearPt address) {
    return mem_readd_inline(address);
}

#include "cpu.h"

extern bool warn_on_mem_write;
extern CPUBlock cpu;

void mem_writeb(LinearPt address,uint8_t val) {
//  if (warn_on_mem_write && cpu.pmode) LOG_MSG("WARNING: post-killswitch memory write to 0x%08x = 0x%02x\n",address,val);
    mem_writeb_inline(address,val);
}

void mem_writew(LinearPt address,uint16_t val) {
//  if (warn_on_mem_write && cpu.pmode) LOG_MSG("WARNING: post-killswitch memory write to 0x%08x = 0x%04x\n",address,val);
    mem_writew_inline(address,val);
}

void mem_writed(LinearPt address,uint32_t val) {
//  if (warn_on_mem_write && cpu.pmode) LOG_MSG("WARNING: post-killswitch memory write to 0x%08x = 0x%08x\n",address,val);
    mem_writed_inline(address,val);
}

void phys_writes(PhysPt addr, const char* string, Bitu length) {
    for(Bitu i = 0; i < length && (addr+i) < MemSize; i++) host_writeb(MemBase+addr+i,(uint8_t)string[i]);
}

#include "control.h"

unsigned char CMOS_GetShutdownByte();
void CPU_Snap_Back_To_Real_Mode();
void DEBUG_Enable(bool pressed);
void CPU_Snap_Back_Forget();
uint16_t CPU_Pop16(void);

static bool cmos_reset_type_9_sarcastic_win31_comments=true;

void On_Software_286_int15_block_move_return(unsigned char code) {
    uint16_t vec_seg,vec_off;

    /* make CPU core stop immediately */
    CPU_Cycles = 0;

    /* force CPU back to real mode */
    CPU_Snap_Back_To_Real_Mode();
    CPU_Snap_Back_Forget();

    /* read the reset vector from the BIOS data area. this time, it's a stack pointer */
    vec_off = phys_readw(0x400 + 0x67);
    vec_seg = phys_readw(0x400 + 0x69);

    if (cmos_reset_type_9_sarcastic_win31_comments) {
        cmos_reset_type_9_sarcastic_win31_comments=false;
        LOG_MSG("CMOS Shutdown byte 0x%02x says to do INT 15 block move reset %04x:%04x. Only weirdos like Windows 3.1 use this... NOT WELL TESTED!",code,vec_seg,vec_off);
    }

    /* set stack pointer. prepare to emulate BIOS returning from INT 15h block move, 286 style */
    CPU_SetSegGeneral(cs,0xF000);
    CPU_SetSegGeneral(ss,vec_seg);
    reg_esp = vec_off;

    /* WARNING! WARNING! This is based on what Windows 3.1 standard mode (cputype=286) expects.
     * We need more comprehensive documentation on what actual 286 BIOSes do.
     * This code, order-wise, is a guess! No documentation exists on what actually happens!
     * But so far, this allows Windows 3.1 to run in full Standard Mode when cputype=286 without crashing.
     * Be warned that while it works DOSBox-X will spit a shitload of messages about the triple-fault reset trick it's using. */
    CPU_SetSegGeneral(es,CPU_Pop16());  /* FIXME: ES? or DS? */
    CPU_SetSegGeneral(ds,CPU_Pop16());  /* FIXME: ES? or DS? */
    /* probably the stack frame of POPA */
    reg_di=CPU_Pop16();reg_si=CPU_Pop16();reg_bp=CPU_Pop16();CPU_Pop16();//Don't save SP
    reg_bx=CPU_Pop16();reg_dx=CPU_Pop16();reg_cx=CPU_Pop16();reg_ax=CPU_Pop16();
    /* and then finally what looks like an IRET frame */
    CPU_IRET(false,0);

    /* force execution change (FIXME: Is there a better way to do this?) */
    throw int(4);
    /* does not return */
}

void On_Software_286_reset_vector(unsigned char code) {
    uint16_t vec_seg,vec_off;

    /* make CPU core stop immediately */
    CPU_Cycles = 0;

    /* force CPU back to real mode */
    CPU_Snap_Back_To_Real_Mode();
    CPU_Snap_Back_Forget();

    /* read the reset vector from the BIOS data area */
    vec_off = phys_readw(0x400 + 0x67);
    vec_seg = phys_readw(0x400 + 0x69);

    /* TODO: If cputype=386 or cputype=486, and A20 gate disabled, treat it as an intentional trick
     * to trigger a reset + invalid opcode exception through which the program can then read the
     * CPU stepping ID register */

    LOG_MSG("CMOS Shutdown byte 0x%02x says to jump to reset vector %04x:%04x",code,vec_seg,vec_off);

    /* following CPU reset, and coming from the BIOS, CPU registers are trashed */
    reg_eax = 0x2010000;
    reg_ebx = 0x2111;
    reg_ecx = 0;
    reg_edx = 0xABCD;
    reg_esi = 0;
    reg_edi = 0;
    reg_ebp = 0;
    reg_esp = 0x4F8;
    CPU_SetSegGeneral(ds,0x0040);
    CPU_SetSegGeneral(es,0x0000);
    CPU_SetSegGeneral(ss,0x0000);
    /* redirect CPU instruction pointer */
    CPU_SetSegGeneral(cs,vec_seg);
    reg_eip = vec_off;

    /* force execution change (FIXME: Is there a better way to do this?) */
    throw int(4);
    /* does not return */
}

void CPU_Exception_Level_Reset();

extern bool custom_bios;
extern bool PC98_SHUT0,PC98_SHUT1;

void On_Software_CPU_Reset() {
    unsigned char c;

    CPU_Exception_Level_Reset();

    if (custom_bios) {
        /* DO NOTHING */
        LOG_MSG("CPU RESET: Doing nothing, custom BIOS loaded");

        if (IS_PC98_ARCH)
            LOG_MSG("CPU RESET: SHUT0=%u SHUT1=%u",PC98_SHUT0,PC98_SHUT1);
        else
            LOG_MSG("CPU RESET: CMOS BYTE 0x%02x",CMOS_GetShutdownByte());
    }
    else if (IS_PC98_ARCH) {
        /* From Undocumented 9801, 9821 Volume 2:
         *
         * SHUT0 | SHUT1 | Meaning
         * -----------------------
         * 1       1       System reset (BIOS performs full reinitialization)
         * 1       0       Invalid (BIOS will show "SYSTEM SHUTDOWN" and stop)
         * 0       x       Continue program execution after CPU reset.
         *                    BIOS loads SS:SP from 0000:0404 and then executes RETF */
        if (PC98_SHUT0) {
            if (!PC98_SHUT1)
                E_Exit("PC-98 invalid reset aka SYSTEM SHUTDOWN (SHUT0=1 SHUT1=0)");
        }
        else { // SHUT0=0 SHUT1=x
            void CPU_Snap_Back_To_Real_Mode();
            void CPU_Snap_Back_Forget();

            /* fake CPU reset */
            CPU_Snap_Back_To_Real_Mode();
            CPU_Snap_Back_Forget();

            /* following CPU reset, and coming from the BIOS, CPU registers are trashed */
            /* FIXME: VEM486.EXE appears to use this reset vector trick, then when regaining control,
             *        checks to see if DX is 0x00F0 just as it was when it issued the OUT DX,AL
             *        instruction. Why? If DX != 0x00F0 it writes whatever DX is to 0000:0486 and
             *        then proceeds anyway. */
            reg_eax = 0x2010000;
            reg_ebx = 0x2111;
            reg_ecx = 0;
            reg_edx = 0xABCD;
            reg_esi = 0;
            reg_edi = 0;
            reg_ebp = 0;
            reg_esp = 0x4F8;
            CPU_SetSegGeneral(ds,0x0040);
            CPU_SetSegGeneral(es,0x0000);
            CPU_SetSegGeneral(ss,0x0000);

            /* continue program execution after CPU reset */
            uint16_t reset_sp = mem_readw(0x404);
            uint16_t reset_ss = mem_readw(0x406);

            LOG_MSG("PC-98 reset and continue: SS:SP = %04x:%04x",reset_ss,reset_sp);

            reg_esp = reset_sp;
            CPU_SetSegGeneral(ss,reset_ss);

            uint16_t new_ip = CPU_Pop16();
            uint16_t new_cs = CPU_Pop16();

            reg_eip = new_ip;
            CPU_SetSegGeneral(cs,new_cs);

            LOG_MSG("PC-98 reset and continue: RETF to %04x:%04x",SegValue(cs),reg_ip);

            /* force execution change (FIXME: Is there a better way to do this?) */
            throw int(4);
            /* does not return */
        }
    }
    else {
        /* IBM reset vector or system reset by CMOS shutdown byte */

        /* software-initiated CPU reset. but the intent may not be to reset the system but merely
         * the CPU. check the CMOS shutdown byte */
        switch (c=CMOS_GetShutdownByte()) {
            case 0x05:  /* JMP double word pointer with EOI */
            case 0x0A:  /* JMP double word pointer without EOI */
                On_Software_286_reset_vector(c);
                return;
            case 0x09:  /* INT 15h block move return to real mode (to appease Windows 3.1 KRNL286.EXE and cputype=286, yuck) */
                On_Software_286_int15_block_move_return(c);
                return;
        }
    }

#if C_DYNAMIC_X86
    /* this technique is NOT reliable when running the dynamic core! */
    if (cpudecoder == &CPU_Core_Dyn_X86_Run || cpudecoder == &CPU_Core_Dynrec_Run) {
        LOG_MSG("Warning: C++ exception method is not compatible with dynamic core when emulating reset");
        RebootLanguage("");
    }
#endif

    throw int(3);
    /* does not return */
}

/* Some PC-98 code uses this register to know if the 16MB "memory hole" is open,
 * instead pf looking at the BIOS data area. Including homebrew development like KOARMADA.EXE */
static IO_ReadHandleObject PC98_43B_memspace_ReadHandler;
static Bitu read_PC98_43B_memspace(Bitu /*port*/,Bitu /*iolen*/) {
        uint8_t r = 0;

        if (isa_memory_hole_15mb || MEM_TotalPages() <= 0xF00/*15MB or less*/)
                { /* used by system */ }
        else
                r |= 0x04; /* normal memory space */

        return r;
}

bool allow_port_92_reset = true;

static void write_p92(Bitu port,Bitu val,Bitu iolen) {
    (void)iolen;//UNUSED
    (void)port;//UNUSED
    memory.a20.controlport = (uint8_t)(val & ~2u);
    MEM_A20_Enable((val & 2u)>0);

    // Bit 0 = system reset (switch back to real mode)
    if (val & 1) {
        if (allow_port_92_reset) {
            LOG_MSG("Restart by port 92h requested\n");
            On_Software_CPU_Reset();
        }
        else {
            LOG_MSG("WARNING: port 92h written with bit 0 set. Is the guest OS or application attempting to reset the system?\n");
        }
    }
}

static Bitu read_p92(Bitu port,Bitu iolen) {
    (void)iolen;//UNUSED
    (void)port;//UNUSED
    return (Bitu)memory.a20.controlport | (memory.a20.enabled ? 0x02u : 0);
}

static Bitu read_pc98_a20(Bitu port,Bitu iolen) {
    (void)iolen;//UNUSED
    if (port == 0xF2)
        return (memory.a20.enabled ? 0x00 : 0x01); // bit 0 indicates whether A20 is MASKED, not ENABLED

    return ~0ul;
}

static void write_pc98_a20(Bitu port,Bitu val,Bitu iolen) {
    (void)iolen;//UNUSED
    if (port == 0xF2) {
        MEM_A20_Enable(1); // writing port 0xF2 unmasks (enables) A20 regardless of value
    }
    else if (port == 0xF6) {
        if ((val & 0xFE) == 0x02) { // A20 gate control 0000 001x  x=mask A20 if set
            MEM_A20_Enable(!(val & 1));
        }
        else {
            LOG_MSG("PC-98 port F6h unknown value 0x%x",(unsigned int)val);
        }
    }
}

void RemoveEMSPageFrame(void) {
    LOG(LOG_MISC,LOG_DEBUG)("Removing EMS page frame");

    if(IS_PC98_ARCH || IS_J3100) {
        for (Bitu ct=0xd0;ct<0xe0;ct++) {
            memory.phandlers[ct] = &rom_page_handler;
        }
    } else {
        /* Setup rom at 0xe0000-0xf0000 */
        for (Bitu ct=0xe0;ct<0xf0;ct++) {
            memory.phandlers[ct] = &rom_page_handler;
        }
    }
}

void PreparePCJRCartRom(void) {
    LOG(LOG_MISC,LOG_DEBUG)("Preparing mapping for PCjr cartridge ROM");

    /* Setup rom at 0xd0000-0xe0000 */
    for (Bitu ct=0xd0;ct<0xe0;ct++) {
        memory.phandlers[ct] = &rom_page_handler;
    }
}

/* how to use: unmap_physmem(0xA0000,0xBFFFF) to unmap 0xA0000 to 0xBFFFF */
bool MEM_unmap_physmem(Bitu start,Bitu end) {
    Bitu p;

    if (start & 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() start not page aligned.\n");
    if ((end & 0xFFF) != 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() end not page aligned.\n");
    start >>= 12; end >>= 12;

    if (start >= memory.handler_pages || end >= memory.handler_pages)
        E_Exit("%s: attempt to map pages beyond handler page limit (0x%lx-0x%lx >= 0x%lx)",
            __FUNCTION__,(unsigned long)start,(unsigned long)end,(unsigned long)memory.handler_pages);

    for (p=start;p <= end;p++)
        memory.phandlers[p] = &unmapped_page_handler;

    PAGING_ClearTLB();
    return true;
}

bool MEM_map_RAM_physmem(Bitu start,Bitu end) {
    Bitu p;
    PageHandler *ram_ptr = (PageHandler*)(&ram_page_handler);

    if (start & 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() start not page aligned.\n");
    if ((end & 0xFFF) != 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() end not page aligned.\n");
    start >>= 12; end >>= 12;

    if (start >= memory.handler_pages || end >= memory.handler_pages)
        E_Exit("%s: attempt to map pages beyond handler page limit (0x%lx-0x%lx >= 0x%lx)",
            __FUNCTION__,(unsigned long)start,(unsigned long)end,(unsigned long)memory.handler_pages);

    for (p=start;p <= end;p++) {
        if (memory.phandlers[p] != NULL && memory.phandlers[p] != &illegal_page_handler &&
            memory.phandlers[p] != &unmapped_page_handler && memory.phandlers[p] != &ram_page_handler)
            return false;
    }

    for (p=start;p <= end;p++)
        memory.phandlers[p] = ram_ptr;

    PAGING_ClearTLB();
    return true;
}

bool MEM_map_ROM_physmem(Bitu start,Bitu end) {
    Bitu p;

    if (start & 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() start not page aligned.\n");
    if ((end & 0xFFF) != 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() end not page aligned.\n");
    start >>= 12; end >>= 12;

    if (start >= memory.handler_pages || end >= memory.handler_pages)
        E_Exit("%s: attempt to map pages beyond handler page limit (0x%lx-0x%lx >= 0x%lx)",
            __FUNCTION__,(unsigned long)start,(unsigned long)end,(unsigned long)memory.handler_pages);

    for (p=start;p <= end;p++) {
        if (memory.phandlers[p] != NULL && memory.phandlers[p] != &illegal_page_handler &&
            memory.phandlers[p] != &unmapped_page_handler && memory.phandlers[p] != &rom_page_handler)
            return false;
    }

    for (p=start;p <= end;p++)
        memory.phandlers[p] = &rom_page_handler;

    PAGING_ClearTLB();
    return true;
}

bool MEM_map_ROM_alias_physmem(Bitu start,Bitu end) {
    Bitu p;

    if (start & 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() start not page aligned.\n");
    if ((end & 0xFFF) != 0xFFF)
        LOG_MSG("WARNING: unmap_physmem() end not page aligned.\n");
    start >>= 12; end >>= 12;

    if (start >= memory.handler_pages || end >= memory.handler_pages)
        E_Exit("%s: attempt to map pages beyond handler page limit (0x%lx-0x%lx >= 0x%lx)",
            __FUNCTION__,(unsigned long)start,(unsigned long)end,(unsigned long)memory.handler_pages);

    for (p=start;p <= end;p++) {
        if (memory.phandlers[p] != NULL && memory.phandlers[p] != &illegal_page_handler && memory.phandlers[p] != &unmapped_page_handler)
            return false;
    }

    for (p=start;p <= end;p++)
        memory.phandlers[p] = &rom_page_alias_handler;

    PAGING_ClearTLB();
    return true;
}

HostPt GetMemBase(void) { return MemBase; }

/*! \brief          REDOS.COM utility command on drive Z: to trigger restart of the DOS kernel
 */
class REDOS : public Program {
public:
    /*! \brief      Program entry point, when the command is run */
    void Run(void) override {
		if (cmd->FindExist("/?", false) || cmd->FindExist("-?", false)) {
			WriteOut("Reboots the kernel of DOSBox-X's emulated DOS.\n\nRE-DOS\n");
			return;
		}
        throw int(6);
    }
};

void REDOS_ProgramStart(Program * * make) {
    *make=new REDOS;
}

/*! \brief          A20GATE.COM built-in command on drive Z:
 *  
 *  \description    Utility command for the user to set/view the A20 gate state
 */
class A20GATE : public Program {
public:
    /*! \brief      Program entry point, when the command is run */
    void Run(void) override {
        if (cmd->FindExist("-?", false) || cmd->FindExist("/?", false)) {
            WriteOut("Turns on/off or changes the A20 gate mode.\n\n");
            WriteOut("A20GATE [ON | OFF | SET [off | off_fake | on | on_fake | mask | fast]]\n\n"
                    "  [ON | OFF | SET] Turns the A20 gate ON/OFF, or sets the A20 gate mode.\n\n"
                    "Type A20GATE with no parameters to display the current A20 gate status.\n");
        }
        else if (cmd->FindString("SET",temp_line,false)) {
            char *x = (char*)temp_line.c_str();

            a20_fast_changeable = false;
            a20_fake_changeable = false;
            a20_guest_changeable = true;
            MEM_A20_Enable(true);

            if (!strncasecmp(x,"off_fake",8)) {
                MEM_A20_Enable(false);
                a20_guest_changeable = false;
                a20_fake_changeable = true;
                WriteOut("A20 gate is now in off_fake mode.\n");
            }
            else if (!strncasecmp(x,"off",3)) {
                MEM_A20_Enable(false);
                a20_guest_changeable = false;
                a20_fake_changeable = false;
                WriteOut("A20 gate is now in off mode.\n");
            }
            else if (!strncasecmp(x,"on_fake",7)) {
                MEM_A20_Enable(true);
                a20_guest_changeable = false;
                a20_fake_changeable = true;
                WriteOut("A20 gate is now in on_fake mode.\n");
            }
            else if (!strncasecmp(x,"on",2)) {
                MEM_A20_Enable(true);
                a20_guest_changeable = false;
                a20_fake_changeable = false;
                WriteOut("A20 gate is now in on mode.\n");
            }
            else if (!strncasecmp(x,"mask",4)) {
                MEM_A20_Enable(false);
                a20_guest_changeable = true;
                a20_fake_changeable = false;
                memory.a20.enabled = 0;
                WriteOut("A20 gate is now in mask mode.\n");
            }
            else if (!strncasecmp(x,"fast",4)) {
                MEM_A20_Enable(false);
                a20_guest_changeable = true;
                a20_fake_changeable = false;
                a20_fast_changeable = true;
                WriteOut("A20 gate is now in fast mode\n");
            }
            else {
                WriteOut("Unknown setting - %s\n", x);
            }
        }
        else if (cmd->FindExist("ON")) {
            WriteOut("Enabling A20 gate...\n");
            MEM_A20_Enable(true);
            if (!MEM_A20_Enabled()) WriteOut("Error: A20 gate cannot be enabled.\n");
        }
        else if (cmd->FindExist("OFF")) {
            WriteOut("Disabling A20 gate...\n");
            MEM_A20_Enable(false);
            if (MEM_A20_Enabled()) WriteOut("Error: A20 gate cannot be disabled.\n");
        }
        else {
            WriteOut("A20 gate is currently %s.\n", MEM_A20_Enabled()?"ON":"OFF");
        }
    }
};

void A20GATE_ProgramStart(Program * * make) {
    *make=new A20GATE;
}

void Init_AddressLimitAndGateMask() {
    Section_prop * section=static_cast<Section_prop *>(control->GetSection("dosbox"));

    LOG(LOG_MISC,LOG_DEBUG)("Initializing address limit/gate system");

    // TODO: this option should be handled by the CPU init since it concerns emulation
    //       of older 386 and 486 boards with fewer than 32 address lines:
    //
    //         24-bit addressing on the 386SX vs full 32-bit addressing on the 386DX
    //         26-bit addressing on the 486SX vs full 32-bit addressing on the 486DX
    //
    //       Also this code should automatically cap itself at 24 for 286 emulation and
    //       20 for 8086 emulation.
    memory.address_bits=(unsigned int)section->Get_int("memalias");

    if (memory.address_bits == 0) {
        /* TODO: We don't know the memsize yet. If memsize is 60GB or more, 40 bits, else 36 bits.
         *       Pentium II/III systems are PSE-36 type PSE extensions.
         *       For similar reasons for 486, if 60MB or more, 32 bits, else 26 bits.
         *       For similar reasons for 386, if 14mB or more, 32 bits, else 24 bits. */
        if (CPU_ArchitectureType >= CPU_ARCHTYPE_PENTIUMII)
            memory.address_bits = 36;
        else if (CPU_ArchitectureType >= CPU_ARCHTYPE_386)
            memory.address_bits = 32; /* NTS: 26 is also valid for 486SX emulation, 24 for 386SX emulation */
        else if (CPU_ArchitectureType >= CPU_ARCHTYPE_286)
            memory.address_bits = 24; /* The 286 cannot address more than 16MB */
        else
            memory.address_bits = 20; /* The 8086 cannot address more than 1MB */
    }
    else if (memory.address_bits < 20)
        memory.address_bits = 20;
    else if (memory.address_bits > 40)
        memory.address_bits = 40;

    // TODO: This should be ...? CPU init? Motherboard init?
    /* WARNING: Binary arithmetic done with 64-bit integers because under Microsoft C++
       ((1UL << 32UL) - 1UL) == 0, which is WRONG.
       But I'll never get back the 4 days I wasted chasing it down, trying to
       figure out why DOSBox was getting stuck reopening its own CON file handle. */
    memory.mem_alias_pagemask = (uint32_t)
        (((((uint64_t)1) << (uint64_t)memory.address_bits) - (uint64_t)1) >> (uint64_t)12);

    /* memory aliasing cannot go below 1MB or serious problems may result. */
    if ((memory.mem_alias_pagemask & 0xFF) != 0xFF) E_Exit("alias pagemask < 1MB");

    /* update alias pagemask according to A20 gate */
    memory.mem_alias_pagemask_active = memory.mem_alias_pagemask;
    if (a20_fake_changeable && !memory.a20.enabled)
        memory.mem_alias_pagemask_active &= ~0x100u;

    /* log */
    LOG(LOG_MISC,LOG_DEBUG)("Memory: address_bits=%u alias_pagemask=%lx",(unsigned int)memory.address_bits,(unsigned long)memory.mem_alias_pagemask);
}

void free_mem_file();

void ShutDownRAM(Section * sec) {
    (void)sec;//UNUSED
    if (MemBase != NULL) {
        if (memory_file_base) {
            assert(MemBase == memory_file_base);
            free_mem_file();
        }
        else {
#if C_GAMELINK
            GameLink::FreeRAM(MemBase);
#elif C_HAVE_MMAP
            munmap(MemBase,MemSize);
#else
            delete [] MemBase;
#endif
        }
        MemBase = NULL;
    }
    MemSize = 0;
    ACPI_free();
}

void MEM_InitCallouts(void) {
    /* make sure each vector has enough for a typical load */
    MEM_callouts[MEM_callouts_index(MEM_TYPE_ISA)].resize(64);
    MEM_callouts[MEM_callouts_index(MEM_TYPE_PCI)].resize(64);
    MEM_callouts[MEM_callouts_index(MEM_TYPE_MB)].resize(64);
}

uint32_t MEM_HardwareAllocate(const char *name,uint32_t sz) {
    uint32_t assign = 0;

    if (sz != 0ul && bitop::ispowerof2(sz)) {
        if (memory.hw_next_assign < 0xFE000000ul) {
            memory.hw_next_assign += sz - 1ul;
            memory.hw_next_assign &= ~(sz - 1ul);
        }
        if (memory.hw_next_assign < 0xFE000000ul) {
            assign = memory.hw_next_assign;
            memory.hw_next_assign += sz;
            LOG(LOG_MISC,LOG_DEBUG)("Device '%s' assigned address 0x%lx-0x%lx which it may treat as minimum\n",name,(unsigned long)assign,(unsigned long)assign+(unsigned long)sz-1ul);
        }
    }

    if (assign == 0)
        LOG(LOG_MISC,LOG_DEBUG)("Unable to assign device '%s' a physical address of size 0x%lx\n",name,(unsigned long)sz);

    return assign;
}

#ifdef DO_MEMORY_FILE
# if C_HAVE_MMAP
void free_mem_file() {
	if (memory_file_base) {
		munmap(memory_file_base,memory_file_size);
		memory_file_base = NULL;
	}
	if (memory_file_fd >= 0) {
		close(memory_file_fd);
		memory_file_fd = -1;
	}
}

bool alloc_mem_file() {
	struct stat st;

	assert(memory_file_fd < 0);
	assert(memory_file_base == NULL);

	if (memory_file.empty() || memory_file_size == 0)
		return false;

	if (lstat(memory_file.c_str(),&st)) {
		if (errno != ENOENT) { /* It's OK if the file doesn't exist yet */
			LOG_MSG("Cannot stat memory file, %s",strerror(errno));
			return false;
		}
	}
	else {
		if (!S_ISREG(st.st_mode)) { /* Must be file! */
			LOG_MSG("Memory file exists and it is not a file");
			return false;
		}
	}

	memory_file_fd = open(memory_file.c_str(),O_CREAT|O_RDWR,0600);
	if (memory_file_fd < 0) {
		LOG_MSG("Cannot open memory file, %s",strerror(errno));
		return false;
	}

	if (fstat(memory_file_fd,&st)) {
		LOG_MSG("Cannot fstat memory file I just opened??? Whut? %s",strerror(errno));
		free_mem_file();
		return false;
	}
	if (!S_ISREG(st.st_mode)) {
		E_Exit("I was tricked into opening a non-file as a memory file. Don't do that.");
		free_mem_file();
		return false;
	}

	if (ftruncate(memory_file_fd,0)) {
		LOG_MSG("Cannot truncate file to zero %s",strerror(errno));
		free_mem_file();
		return false;
	}

	if (ftruncate(memory_file_fd,memory_file_size)) {
		LOG_MSG("Cannot truncate file to %lu %s",(unsigned long)memory_file_size,strerror(errno));
		free_mem_file();
		return false;
	}

	memory_file_base = mmap(NULL/*no particular address*/,memory_file_size,PROT_READ|PROT_WRITE,MAP_SHARED,memory_file_fd,0/*offset*/);
	if (memory_file_base == MAP_FAILED) {
		LOG_MSG("Unable to memory map memory file, %s",strerror(errno));
		memory_file_base = NULL; /* MAP_FAILED might be some nonzero value, such as on Linux where it is -1 */
		free_mem_file();
		return false;
	}

	LOG_MSG("Using memory file '%s' as guest memory",memory_file.c_str());
	memory_file_already_zero = true;
	return true;
}
# elif defined(WIN32_MMAP)
void free_mem_file() {
    if(memory_file_base != NULL) {
        if(UnmapViewOfFile(memory_file_base) == 0) E_Exit("Windows refused to unmap the file view");
        memory_file_base = NULL;
    }
    if(memory_file_map != INVALID_HANDLE_VALUE && memory_file_map != 0) {
        if(CloseHandle(memory_file_map) == 0) E_Exit("Windows refused to close the memory file, err=0x%08x",(unsigned int)GetLastError());
        memory_file_map = INVALID_HANDLE_VALUE;
    }
    if(memory_file_fd != INVALID_HANDLE_VALUE) {
        if(CloseHandle(memory_file_fd) == 0) E_Exit("Windows refused to close the memory file, err=0x%08x", (unsigned int)GetLastError());
        memory_file_fd = INVALID_HANDLE_VALUE;
    }
}

bool alloc_mem_file() {
    assert(memory_file_fd == INVALID_HANDLE_VALUE);
    assert(memory_file_map == INVALID_HANDLE_VALUE);
    assert(memory_file_base == NULL);

    if(memory_file.empty() || memory_file_size == 0)
        return false;

    DWORD attr, err;

    attr = GetFileAttributesA(memory_file.c_str());
    if(attr == INVALID_FILE_ATTRIBUTES) {
        err = GetLastError();
        if(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            /* OK */
        }
        else {
            return false;
        }
    }
    else if(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DEVICE)) {
        free_mem_file();
        return false;
    }

    memory_file_fd = CreateFile(memory_file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
    if(memory_file_fd == INVALID_HANDLE_VALUE) {
        free_mem_file();
        return false;
    }

    if(SetFilePointer(memory_file_fd, 0, 0, FILE_BEGIN) != 0) {
        free_mem_file();
        return false;
    }
    if(SetEndOfFile(memory_file_fd) == 0) {
        free_mem_file();
        return false;
    }

    {
        FILE_SET_SPARSE_BUFFER sp;
        DWORD retval;

        sp.SetSparse = TRUE;
        if(DeviceIoControl(memory_file_fd, FSCTL_SET_SPARSE, &sp, sizeof(sp), NULL, 0, &retval, NULL) == 0)
            LOG_MSG("WARNING: Could not make memory file sparse");
    }

    {
        LONG hi = (LONG)(memory_file_size >> 32ull);
        if(SetFilePointer(memory_file_fd, (DWORD)memory_file_size, &hi, FILE_BEGIN) != (DWORD)memory_file_size) {
            free_mem_file();
            return false;
        }
    }
    if(SetEndOfFile(memory_file_fd) == 0) {
        free_mem_file();
        return false;
    }

    memory_file_map = CreateFileMapping(memory_file_fd, NULL, PAGE_READWRITE, (DWORD)(memory_file_size >> 32ull), (DWORD)memory_file_size, NULL);
    if(memory_file_map == INVALID_HANDLE_VALUE || memory_file_map == 0) {
        const DWORD err = GetLastError();
        free_mem_file();
        return false;
    }

    memory_file_base = MapViewOfFile(memory_file_map, FILE_MAP_ALL_ACCESS, 0, 0, memory_file_size);
    if(memory_file_base == NULL) {
        const DWORD err = GetLastError();
        free_mem_file();
        return false;
    }

    LOG_MSG("Using memory file '%s' as guest memory", memory_file.c_str());
    memory_file_already_zero = true;
    return true;
}
# else
void free_mem_file() {
}

bool alloc_mem_file() {
	return false;
}
# endif
#else
void free_mem_file() {
}

bool alloc_mem_file() {
	return false;
}
#endif

void Init_RAM() {
    Section_prop * section=static_cast<Section_prop *>(control->GetSection("dosbox"));
    Bitu i;

    /* please let me know about shutdown! */
    if (!has_Init_RAM) {
        AddExitFunction(AddExitFunctionFuncPair(ShutDownRAM));
        has_Init_RAM = true;
    }

    /* prepare callouts */
    MEM_InitCallouts();

    // LOG
    LOG(LOG_MISC,LOG_DEBUG)("Initializing RAM emulation (system memory)");

    // CHECK: address mask init must have been called!
    assert(memory.mem_alias_pagemask >= 0xFF);

    {
        const char *str = section->Get_string("memory file");
        memory_file = str;
    }

    /* Setup the Physical Page Links */
    uint64_t memsizekb4gb = 0;
    uint64_t memsizekb = (uint64_t)section->Get_int("memsizekb");
    {
        uint64_t memsize = (uint64_t)section->Get_int("memsize");

        if (memsizekb == 0 && memsize < 1) memsize = 1;
        else if (memsizekb != 0 && (Bits)memsize < 0) memsize = 0;

        /* round up memsizekb to 4KB multiple */
        memsizekb = (memsizekb + 3ull) & (~3ull);

        /* roll memsize into memsizekb, simplify this code */
        memsizekb += memsize * (uint64_t)1024ull;
    }

    /* we can't have more memory than the memory aliasing allows */
    if ((memory.mem_alias_pagemask+1) != 0/*32-bit integer overflow avoidance*/) {
        uint64_t maxmem;

        if (memory.address_bits >= 30) /* 1GB+ */
            maxmem = (memory.mem_alias_pagemask+1) - 0x100; /* minus 64MB */
        else if (memory.address_bits >= 24) /* 16MB+ */
            maxmem = (memory.mem_alias_pagemask+1) - 0x100; /* minus 1MB */
        else
            maxmem = (memory.mem_alias_pagemask+1) - 0x10; /* minus 64KB */

        if ((memsizekb/4) > maxmem) {
            LOG_MSG("%u-bit memory aliasing limits you to %uKB",
                (int)memory.address_bits,(int)maxmem*4);
            if (memory.address_bits <= 32) LOG_MSG("If you are attempting more than 4GB of RAM, you need to set memalias to a value larger than 32");
            memsizekb = maxmem*4;
        }
    }

    {
        uint32_t maxsz32 = 0xF8000000ul;
        uint64_t maxsz;

        static_assert( sizeof(size_t) >= sizeof(void*), "why is size_t smaller than a pointer?" );

        /* Leave 128MB of space at the top for the BIOS, S3 VGA, and Voodoo 3Dfx emulation.
         * There was a known bug 2024/12/21 where setting the maximum memory size and installing
         * Windows XP caused problems because XP would try to use the Voodoo 3Dfx MMIO as memory
         * when enabled at 0xD0000000.
         *
         * BIOS: Given the 512KB at the top, including for ACPI structures
         * PC-98 PEGC framebuffer: 512KB below BIOS
         * S3 LFB and MMIO: 32MB at 32MB alignment
         * Voodoo 3Dfx: 16MB at 16MB alignment */
        /* 2024/12/25: We now allow 4GB or more of RAM! But, to make it work in this codebase,
         *             it has to be divided into a region below 4GB and a region above 4GB. */
        if (!build_memlimit_32bit()) // 64-bit address space
            maxsz = (uint64_t)(1048576ull * 1024ull); // 1TB
        else
            maxsz = (uint64_t)(1024ull * 1024ull); // 1GB

        LOG_MSG("Max %lu sz %lu\n",(unsigned long)maxsz,(unsigned long)memsizekb);
        if (memsizekb > maxsz) {
            LOG_MSG("Maximum memory size is %luKB",(unsigned long)maxsz);
            memsizekb = maxsz;
        }
        LOG_MSG("Final %lu\n",(unsigned long)memsizekb);

        /* 4GB or more requires dividing it into below 4GB and above 4GB.
         * This codebase for the most part is only designed for memory and MMIO
         * below 4GB (32-bit system limits) */
        if (memory.address_bits > 32 && memsizekb > (uint64_t)(maxsz32>>10ull)) {
            memsizekb4gb = memsizekb - (uint64_t)(maxsz32>>10ull);
            memsizekb = (uint64_t)(maxsz32>>10ull);
        }
        else {
            memsizekb4gb = 0;
        }

        LOG_MSG("Final arrangement: Below 4GB = %lluKB, Above 4GB = %lluKB\n",
            (unsigned long long)memsizekb,(unsigned long long)memsizekb4gb);
    }
    memory.reported_pages_4gb = memsizekb4gb/4;
    memory.reported_pages = memory.pages = memsizekb/4;
    memory.hw_next_assign = (uint32_t)memory.pages << 12ul;
    LOG(LOG_MISC,LOG_DEBUG)("Hardware assignment will begin at 0x%lx",(unsigned long)memory.hw_next_assign);

    // FIXME: Hopefully our refactoring will remove the need for this hack
    /* if the config file asks for less than 1MB of memory
     * then say so to the DOS program. but way too much code
     * here assumes memsize >= 1MB */
    if (memory.pages < ((1024*1024)/4096))
        memory.pages = ((1024*1024)/4096);

    // DEBUG message
    LOG(LOG_MISC,LOG_DEBUG)("Memory: %u pages (%uKB) of RAM, %u (%uKB) reported to OS, %u (0x%x) (%uKB) pages of memory handlers",
        (unsigned int)memory.pages,
        (unsigned int)memory.pages*4,
        (unsigned int)memory.reported_pages,
        (unsigned int)memory.reported_pages*4,
        (unsigned int)memory.handler_pages,
        (unsigned int)memory.handler_pages,
        (unsigned int)memory.handler_pages*4);

    // sanity check!
    assert(memory.handler_pages >= memory.pages);
    assert(memory.reported_pages <= memory.pages);
    assert(memory.handler_pages >= memory.reported_pages);
    assert(memory.handler_pages >= 0x100); /* enough for at minimum 1MB of addressable memory */

    /* Allocate the RAM. We alloc as a large unsigned char array. new[] does not initialize the array,
     * so we then must zero the buffer. */
    memory_file_size = size_t(memory.pages) * size_t(4096u);
    if (memory.reported_pages_4gb > 0 && sizeof(void*) > 4) {
        size_t noff = size_t(0x100000000ul) + (size_t(4096u) * size_t(memory.reported_pages_4gb));
        if (memory_file_size < noff) memory_file_size = noff;
    }
    if (!memory_file.empty()) LOG_MSG("Memory file size will be %lluKB",(unsigned long long)memory_file_size >> 10ull);
    if (alloc_mem_file()) {
        MemBase = (uint8_t*)memory_file_base;
#if C_GAMELINK
        LOG_MSG("WARNING: Memory file overrides Game Link memory interface");
#endif
    }
    else {
        if (memory.reported_pages_4gb != 0) {
            LOG_MSG("Memory above 4GB is not supported if not using a memory file");
            memory.reported_pages_4gb = 0;
            memsizekb4gb = 0;
        }
#if C_GAMELINK
        MemBase = GameLink::AllocRAM(memory.pages*4096);
#elif C_HAVE_MMAP
        MemBase = (uint8_t*)mmap(NULL,memory.pages*4096u,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,0,0);
        if (MemBase == (uint8_t*)MAP_FAILED) E_Exit("Failed to mmap allocate memory");
#else // C_GAMELINK
        MemBase = new(std::nothrow) uint8_t[memory.pages*4096];
#endif // C_GAMELINK
    }
    MemSize = size_t(memory.pages*4096);
    if (!MemBase) E_Exit("Can't allocate main memory of %d KB",(int)memsizekb);
    /* Clear the memory, as new doesn't always give zeroed memory
     * (Visual C debug mode). We want zeroed memory though. */
    if (memory_file_base && memory_file_already_zero) {
        LOG_MSG("Host OS should treat memory map as all zeros, skipping memory clear");
    }
    else {
        memset((void*)MemBase,0,memory.reported_pages*4096);
    }
    /* the rest of "ROM" is for unmapped devices so we need to fill it appropriately */
    if (memory.reported_pages < memory.pages)
        memset((char*)MemBase+(memory.reported_pages*4096),0xFF,
            (memory.pages - memory.reported_pages)*4096);
    /* adapter ROM */
    memset((char*)MemBase+0xA0000,0xFF,0x60000);
    /* except for 0xF0000-0xFFFFF */
    memset((char*)MemBase+0xF0000,0x00,0x10000);

    // sanity check. if this condition is false the loops below will overrun the array!
    assert(memory.reported_pages <= memory.handler_pages);

    PageHandler *ram_ptr = (PageHandler*)(&ram_page_handler);

    for (i=0;i < memory.reported_pages;i++)
        memory.phandlers[i] = ram_ptr;
    for (;i < memory.handler_pages;i++)
        memory.phandlers[i] = NULL;//&illegal_page_handler;

    /* ISA 15MB memory hole? */
    if (isa_memory_hole_15mb) {
        for (i=0xf00;i <= 0xfff && i < memory.handler_pages;i++)
            memory.phandlers[i] = NULL;//&illegal_page_handler;
    }

    /* FIXME: VGA emulation will selectively respond to 0xA0000-0xBFFFF according to the video mode,
     *        what we want however is for the VGA emulation to assign illegal_page_handler for
     *        address ranges it is not responding to when mapping changes. */
    for (i=0xa0;i<0x100;i++) /* we want to make sure adapter ROM is unmapped entirely! */
        memory.phandlers[i] = NULL;//&unmapped_page_handler;
}

/* ROM BIOS emulation will call this to impose an additional cap on RAM
 * to make sure the upper alias of the ROM BIOS has room. */
void MEM_cut_RAM_up_to(Bitu addr) {
    Bitu pages = addr >> 12ul;

    if (memory.reported_pages > pages) {
        LOG(LOG_MISC,LOG_DEBUG)("Memory: Reducing RAM to 0x%lx",(unsigned long)addr);

        do { memory.phandlers[--memory.reported_pages] = NULL;
        } while (memory.reported_pages > pages);
    }
}

static IO_ReadHandleObject PS2_Port_92h_ReadHandler;
static IO_WriteHandleObject PS2_Port_92h_WriteHandler;
static IO_WriteHandleObject PS2_Port_92h_WriteHandler2;

void ShutDownMemoryAccessArray(Section * sec) {
    (void)sec;//UNUSED
    if (memory.phandlers != NULL) {
        delete [] memory.phandlers;
        memory.phandlers = NULL;
    }
}

void XMS_ShutDown(Section* /*sec*/);

void ShutDownMemHandles(Section * sec) {
    /* XMS relies on us, so shut it down first to avoid spurious warnings about freeing when mhandles == NULL */
    XMS_ShutDown(NULL);

    (void)sec;//UNUSED
    if (memory.mhandles != NULL) {
        delete [] memory.mhandles;
        memory.mhandles = NULL;
    }
}

/* this is called on hardware reset. the BIOS needs the A20 gate ON to boot properly on 386 or higher!
 * this temporarily switches on the A20 gate and lets it function as normal despite user settings. BIOS
 * will POST and then permit the A20 gate to go back to whatever emulation setting given in dosbox-x.conf */
void A20Gate_OnReset(Section *sec) {
    (void)sec;//UNUSED
    void A20Gate_OverrideOn(Section *sec);

    memory.a20.controlport = 0;
    A20Gate_OverrideOn(sec);
    MEM_A20_Enable(true);
}

void A20Gate_OverrideOn(Section *sec) {
    (void)sec;//UNUSED
    memory.a20.enabled = 1;
    mainMenu.get_item("enable_a20gate").check(true).refresh_item(mainMenu);
    a20_fake_changeable = false;
    a20_guest_changeable = true;
}

/* this is called after BIOS boot. the BIOS needs the A20 gate ON to boot properly on 386 or higher! */
void A20Gate_TakeUserSetting(Section *sec) {
    (void)sec;//UNUSED
    Section_prop * section=static_cast<Section_prop *>(control->GetSection("dosbox"));

    memory.a20.enabled = 0;
    a20_fake_changeable = false;
    a20_guest_changeable = true;
    a20_fast_changeable = false;

    // TODO: A20 gate control should also be handled by a motherboard init routine
    std::string ss = section->Get_string("a20");
    if (ss == "mask" || ss == "") {
        LOG(LOG_MISC,LOG_DEBUG)("A20: masking emulation");
        a20_guest_changeable = true;
    }
    else if (ss == "on") {
        LOG(LOG_MISC,LOG_DEBUG)("A20: locked on");
        a20_guest_changeable = false;
        memory.a20.enabled = 1;
    }
    else if (ss == "on_fake") {
        LOG(LOG_MISC,LOG_DEBUG)("A20: locked on (but will fake control bit)");
        a20_guest_changeable = false;
        a20_fake_changeable = true;
        memory.a20.enabled = 1;
    }
    else if (ss == "off") {
        LOG(LOG_MISC,LOG_DEBUG)("A20: locked off");
        a20_guest_changeable = false;
        memory.a20.enabled = 0;
    }
    else if (ss == "off_fake") {
        LOG(LOG_MISC,LOG_DEBUG)("A20: locked off (but will fake control bit)");
        a20_guest_changeable = false;
        a20_fake_changeable = true;
        memory.a20.enabled = 0;
    }
    else if (ss == "fast") {
        LOG(LOG_MISC,LOG_DEBUG)("A20: fast mode");
        a20_fast_changeable = true;
        a20_guest_changeable = true;
    }
    else {
        LOG(LOG_MISC,LOG_DEBUG)("A20: masking emulation");
        a20_guest_changeable = true;
    }
    mainMenu.get_item("enable_a20gate").check(memory.a20.enabled).refresh_item(mainMenu);
}

void Init_A20_Gate() {
    LOG(LOG_MISC,LOG_DEBUG)("Initializing A20 gate emulation");

    AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(A20Gate_OnReset));
}

void PS2Port92_OnReset(Section *sec) {
    (void)sec;//UNUSED
    Section_prop * section=static_cast<Section_prop *>(control->GetSection("dosbox"));

    PC98_43B_memspace_ReadHandler.Uninstall();
    PS2_Port_92h_WriteHandler2.Uninstall();
    PS2_Port_92h_WriteHandler.Uninstall();
    PS2_Port_92h_ReadHandler.Uninstall();

    if (IS_PC98_ARCH) {
        // TODO: add separate dosbox-x.conf variable for A20 gate control on PC-98
        enable_port92 = true;
        if (enable_port92) {
            PS2_Port_92h_WriteHandler2.Install(0xF6,write_pc98_a20,IO_MB);
            PS2_Port_92h_WriteHandler.Install(0xF2,write_pc98_a20,IO_MB);
            PS2_Port_92h_ReadHandler.Install(0xF2,read_pc98_a20,IO_MB);
        }

        PC98_43B_memspace_ReadHandler.Install(0x43B,read_PC98_43B_memspace,IO_MB);
    }
    else {
        // TODO: this should be handled in a motherboard init routine
        enable_port92 = section->Get_bool("enable port 92");
        if (enable_port92) {
            // A20 Line - PS/2 system control port A
            // TODO: This should exist in the motherboard emulation code yet to come! The motherboard
            //       determines A20 gating, not the RAM!
            LOG(LOG_MISC,LOG_DEBUG)("Port 92h installed, emulating PS/2 system control port A");
            PS2_Port_92h_WriteHandler.Install(0x92,write_p92,IO_MB);
            PS2_Port_92h_ReadHandler.Install(0x92,read_p92,IO_MB);
        }
    }
}

void Init_PS2_Port_92h() {
    LOG(LOG_MISC,LOG_DEBUG)("Initializing PS/2 port 92h emulation");

    AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(PS2Port92_OnReset));
}

void Init_MemHandles() {
    Bitu i;

    if (!has_Init_MemHandles) {
        AddExitFunction(AddExitFunctionFuncPair(ShutDownMemHandles));
        has_Init_MemHandles = true;
    }

    // LOG
    LOG(LOG_MISC,LOG_DEBUG)("Initializing memory handle array (EMS/XMS handle management). mem_pages=%lx",(unsigned long)memory.pages);

    if (memory.mhandles == NULL)
        memory.mhandles = new MemHandle[memory.pages];

    for (i = 0;i < memory.pages;i++)
        memory.mhandles[i] = 0;             //Set to 0 for memory allocation

    // ISA memory hole awareness (15MB region). Block off 0xF00000-0xFFFFFF with a dummy handle.
    if (isa_memory_hole_15mb) {
        for (i=0xF00;i<=0xFFF && i < memory.pages;i++) memory.mhandles[i] = 0x7FFFFFFF;
    }
}

void Init_MemoryAccessArray() {
    Bitu i;

    /* HACK: need to zero these! */
    memory.lfb.handler=NULL;
    memory.lfb.start_page=0;
    memory.lfb.end_page=0;
    memory.lfb.pages=0;

    memory.lfb_mmio.handler=NULL;
    memory.lfb_mmio.start_page=0;
    memory.lfb_mmio.end_page=0;
    memory.lfb_mmio.pages=0;

    if (!has_Init_MemoryAccessArray) {
        has_Init_MemoryAccessArray = true;
        AddExitFunction(AddExitFunctionFuncPair(ShutDownMemoryAccessArray));
    }

    // LOG
    LOG(LOG_MISC,LOG_DEBUG)("Initializing memory access array (page handler callback system). mem_alias_pagemask=%lx",(unsigned long)memory.mem_alias_pagemask);

    // CHECK: address mask init must have been called!
    assert(memory.mem_alias_pagemask >= 0xFF);

    // we maintain a different page count for page handlers because we want to maintain a
    // "cache" of sorts of what device responds to a given memory address.
    memory.handler_pages = (1 << (32 - 12)); /* enough for 4GB */
    if ((memory.mem_alias_pagemask+1) != 0/*integer overflow check*/ &&
        memory.handler_pages > (memory.mem_alias_pagemask+1))
        memory.handler_pages = (memory.mem_alias_pagemask+1);

    if (memory.phandlers == NULL)
        memory.phandlers = new PageHandler* [memory.handler_pages];

    for (i=0;i < memory.handler_pages;i++) // FIXME: This will eventually init all pages to the "slow path" for device lookup
        memory.phandlers[i] = NULL;//&illegal_page_handler;
}

void Init_PCJR_CartridgeROM() {
    Bitu i;

    // log
    LOG(LOG_MISC,LOG_DEBUG)("Mapping ROM handler for PCjr cartridge emulation");

    /* Setup cartridge rom at 0xe0000-0xf0000.
     * Don't call this function unless emulating PCjr! */
    for (i=0xe0;i<0xf0;i++)
        memory.phandlers[i] = &rom_page_handler;
}

Bitu MEM_PageMask(void) {
    return memory.mem_alias_pagemask;
}

Bitu MEM_PageMaskActive(void) {
    return memory.mem_alias_pagemask_active;
}

// Physical DEVICE access. This is different from phys_readb/phys_writeb because
// those functions can only access system RAM, and is not affected by any device
// mappings or page tables.

uint8_t physdev_readb(const PhysPt64 addr) {
	const PageNum pagenum = (PageNum)(addr >> (PhysPt64)12u);
	PageHandler *ph = MEM_GetPageHandler(pagenum);

	if (ph->flags & PFLAG_READABLE)
		return *((uint8_t*)(ph->GetHostReadPt(pagenum)+((unsigned int)addr&0xFFFu)));

	/* This hack is necessary because of the weird way that CPU linear addresses
	 * make their way down to the hardware read/write callbacks */
	const uint32_t orig = paging.tlb.phys_page[pagenum];
	paging.tlb.phys_page[pagenum] = (uint32_t)pagenum;
	const uint8_t ch = ph->readb((PhysPt)addr); /* WARNING: 4GB wraparound here */
	paging.tlb.phys_page[pagenum] = orig;
	return ch;
}

uint16_t physdev_readw(const PhysPt64 addr) {
	if (((unsigned int)addr&0xFFFu) <= 0xFFEu) {
		const PageNum pagenum = (PageNum)(addr >> (PhysPt64)12u);
		PageHandler *ph = MEM_GetPageHandler(pagenum);

		if (ph->flags & PFLAG_READABLE)
			return *((uint16_t*)(ph->GetHostReadPt(pagenum)+((unsigned int)addr&0xFFFu)));

		/* This hack is necessary because of the weird way that CPU linear addresses
		 * make their way down to the hardware read/write callbacks */
		const uint32_t orig = paging.tlb.phys_page[pagenum];
		paging.tlb.phys_page[pagenum] = (uint32_t)pagenum;
		const uint16_t ch = ph->readw((PhysPt)addr); /* WARNING: 4GB wraparound here */
		paging.tlb.phys_page[pagenum] = orig;
		return ch;
	}
	else {
		return	 (uint16_t)physdev_readb(addr) +
			((uint16_t)physdev_readb(addr+1u) << (uint16_t)8u);
	}
}

uint32_t physdev_readd(const PhysPt64 addr) {
	if (((unsigned int)addr&0xFFFu) <= 0xFFCu) {
		const PageNum pagenum = (PageNum)(addr >> (PhysPt64)12u);
		PageHandler *ph = MEM_GetPageHandler(pagenum);

		if (ph->flags & PFLAG_READABLE)
			return *((uint32_t*)(ph->GetHostReadPt(pagenum)+((unsigned int)addr&0xFFFu)));

		/* This hack is necessary because of the weird way that CPU linear addresses
		 * make their way down to the hardware read/write callbacks */
		const uint32_t orig = paging.tlb.phys_page[pagenum];
		paging.tlb.phys_page[pagenum] = (uint32_t)pagenum;
		const uint32_t ch = ph->readd((PhysPt)addr); /* WARNING: 4GB wraparound here */
		paging.tlb.phys_page[pagenum] = orig;
		return ch;
	}
	else {
		return	 (uint32_t)physdev_readb(addr) +
			((uint32_t)physdev_readb(addr+1u) << (uint32_t)8u) +
			((uint32_t)physdev_readb(addr+2u) << (uint32_t)16u) +
			((uint32_t)physdev_readb(addr+3u) << (uint32_t)24u);
	}
}

void physdev_writeb(const PhysPt64 addr,const uint8_t val) {
	const PageNum pagenum = (PageNum)(addr >> (PhysPt64)12u);
	PageHandler *ph = MEM_GetPageHandler(pagenum);

	if (ph->flags & PFLAG_WRITEABLE) {
		*((uint8_t*)(ph->GetHostReadPt(pagenum)+((unsigned int)addr&0xFFFu))) = val;
	}
	else {
		/* This hack is necessary because of the weird way that CPU linear addresses
		 * make their way down to the hardware read/write callbacks */
		const uint32_t orig = paging.tlb.phys_page[pagenum];
		paging.tlb.phys_page[pagenum] = (uint32_t)pagenum;
		ph->writeb((PhysPt)addr,val); /* WARNING: 4GB wraparound here */
		paging.tlb.phys_page[pagenum] = orig;
	}
}

void physdev_writew(const PhysPt64 addr,const uint16_t val) {
	if (((unsigned int)addr&0xFFFu) <= 0xFFEu) {
		const PageNum pagenum = (PageNum)(addr >> (PhysPt64)12u);
		PageHandler *ph = MEM_GetPageHandler(pagenum);

		if (ph->flags & PFLAG_WRITEABLE) {
			*((uint16_t*)(ph->GetHostWritePt(pagenum)+((unsigned int)addr&0xFFFu))) = val;
		}
		else {
			/* This hack is necessary because of the weird way that CPU linear addresses
			 * make their way down to the hardware read/write callbacks */
			const uint32_t orig = paging.tlb.phys_page[pagenum];
			paging.tlb.phys_page[pagenum] = (uint32_t)pagenum;
			ph->writew((PhysPt)addr,val); /* WARNING: 4GB wraparound here */
			paging.tlb.phys_page[pagenum] = orig;
		}
	}
	else {
		physdev_writeb(addr+0u,val);
		physdev_writeb(addr+1u,val >> 8u);
	}
}

void physdev_writed(const PhysPt64 addr,const uint32_t val) {
	if (((unsigned int)addr&0xFFFu) <= 0xFFCu) {
		const PageNum pagenum = (PageNum)(addr >> (PhysPt64)12u);
		PageHandler *ph = MEM_GetPageHandler(pagenum);

		if (ph->flags & PFLAG_WRITEABLE) {
			*((uint32_t*)(ph->GetHostWritePt(pagenum)+((unsigned int)addr&0xFFFu))) = val;
		}
		else {
			/* This hack is necessary because of the weird way that CPU linear addresses
			 * make their way down to the hardware read/write callbacks */
			const uint32_t orig = paging.tlb.phys_page[pagenum];
			paging.tlb.phys_page[pagenum] = (uint32_t)pagenum;
			ph->writed((PhysPt)addr,val); /* WARNING: 4GB wraparound here */
			paging.tlb.phys_page[pagenum] = orig;
		}
	}
	else {
		physdev_writeb(addr+0u,val);
		physdev_writeb(addr+1u,val >> 8u);
		physdev_writeb(addr+2u,val >> 16u);
		physdev_writeb(addr+3u,val >> 24u);
	}
}

//save state support
extern void* VGA_PageHandler_Func[16];

void *Memory_PageHandler_table[] =
{
	NULL,
	&ram_page_handler,
	&rom_page_handler,

	VGA_PageHandler_Func[0],
	VGA_PageHandler_Func[1],
	VGA_PageHandler_Func[2],
	VGA_PageHandler_Func[3],
	VGA_PageHandler_Func[4],
	VGA_PageHandler_Func[5],
	VGA_PageHandler_Func[6],
	VGA_PageHandler_Func[7],
	VGA_PageHandler_Func[8],
	VGA_PageHandler_Func[9],
	VGA_PageHandler_Func[10],
	VGA_PageHandler_Func[11],
	VGA_PageHandler_Func[12],
	VGA_PageHandler_Func[13],
	VGA_PageHandler_Func[14],
	VGA_PageHandler_Func[15],
};

extern bool dos_kernel_disabled;

namespace
{
class SerializeMemory : public SerializeGlobalPOD
{
public:
	SerializeMemory() : SerializeGlobalPOD("Memory")
	{}

private:
	void getBytes(std::ostream& stream) override
	{
		uint8_t pagehandler_idx[0x40000];
		unsigned int size_table;

		// Assume 1GB maximum memory size
		// FIXME: Memory size can be even larger! Up to 3.5GB on 64-bit builds!
		size_table = sizeof(Memory_PageHandler_table) / sizeof(void *);
		for( unsigned int lcv=0; lcv<memory.pages; lcv++ ) {
			pagehandler_idx[lcv] = 0xff;

			for( unsigned int lcv2=0; lcv2<size_table; lcv2++ ) {
				if( memory.phandlers[lcv] == Memory_PageHandler_table[lcv2] ) {
					pagehandler_idx[lcv] = lcv2;
					break;
				}
			}
		}

		//*******************************************
		//*******************************************

		SerializeGlobalPOD::getBytes(stream);

		// - near-pure data
		WRITE_POD( &memory, memory );

		// - static 'new' ptr
		WRITE_POD_SIZE( MemBase, memory.pages*4096 );

		//***********************************************
		//***********************************************

		if (!dos_kernel_disabled) {
			WRITE_POD_SIZE( memory.mhandles, sizeof(MemHandle) * memory.pages );
		}
		else {
			/* gotta fake it! */
			MemHandle m = 0;
			for (unsigned int i=0;i < memory.pages;i++) {
				WRITE_POD_SIZE( &m, sizeof(MemHandle) );
			}
		}
		WRITE_POD( &pagehandler_idx, pagehandler_idx );
	}

	void setBytes(std::istream& stream) override
	{
		uint8_t pagehandler_idx[0x40000];
		void *old_ptrs[4];

		old_ptrs[0] = (void *) memory.phandlers;
		old_ptrs[1] = (void *) memory.mhandles;
		old_ptrs[2] = (void *) memory.lfb.handler;
		old_ptrs[3] = (void *) memory.lfb_mmio.handler;

		//***********************************************
		//***********************************************

		SerializeGlobalPOD::setBytes(stream);


		// - near-pure data
		READ_POD( &memory, memory );

		// - static 'new' ptr
		READ_POD_SIZE( MemBase, memory.pages*4096 );

		//***********************************************
		//***********************************************

		memory.phandlers = (PageHandler **) old_ptrs[0];
		memory.mhandles = (MemHandle *) old_ptrs[1];
		memory.lfb.handler = (PageHandler *) old_ptrs[2];
		memory.lfb_mmio.handler = (PageHandler *) old_ptrs[3];


		if (!dos_kernel_disabled) {
			READ_POD_SIZE( memory.mhandles, sizeof(MemHandle) * memory.pages );
		}
		else {
			/* gotta fake it! */
			MemHandle m = 0;
			for (unsigned int i=0;i < memory.pages;i++) {
				READ_POD_SIZE( &m, sizeof(MemHandle) );
			}
		}
		READ_POD( &pagehandler_idx, pagehandler_idx );


		for( unsigned int lcv=0; lcv<memory.pages; lcv++ ) {
			if( pagehandler_idx[lcv] != 0xff )
				memory.phandlers[lcv] = (PageHandler *) Memory_PageHandler_table[ pagehandler_idx[lcv] ];
			else if ( lcv >= 0xa0 && lcv <= 0xff)
				{ /* VGA and BIOS emulation does not handle this right, yet */ }
			else
				memory.phandlers[lcv] = NULL; /* MEM_SlowPath() will fill it in again */
		}
	}
} dummy;
}
