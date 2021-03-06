/*

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
//
// @file:
//      page.c
//
// @description:
//      Kernel paging and page allocation.
//      Partially derived from :=
//
//      mem_size() is from
//      GazOS Operating System
//      Copyright (C) 1999  Gareth Owen <gaz@athene.co.uk>
//
//      Code was re-designed and written to work with malloc.c/kmalloc.c
//      by Dr. Roger G. Doss, PhD
//
// @design:
//      This works by allocating as many page tables as there are real 
//      pages of RAM. The memory is split into kernel memory and user memory
//      although it works off of one common kernel page directory.
//      There is some book keeping needed, namely, we have
//      the kernel loaded into _K_BASE by the loader; however,
//      it is then relocated to address 0x100000 and is
//      `size vmox` in length. This is the size of the process
//      in memory once it is loaded. Currently, this is
//      319580. Thus, we need 0x100000 + ONE_MEG which is the size
//      of KERNEL_END (reserving one meg for the kernel). 
//      We then have the bytes needed to store the
//      page tables and the bytes needed to store the bitmaps.
//      Bitmaps are used to mark which page is free/available.
//      Page tables are used to mark which pages are kernel or user.
//      The kernel will occupy the lower end of ram while the user
//      will occupy the higher end of ram. The bitmaps for the lower ram
//      up to where the kernel memory starts must be marked as allocated.
//
//      Allocating pages is then a matter of scanning for a run of free
//      pages in either kernel space (kpage_alloc) or user space (page_alloc)
//      and returning the address to the caller. We also implement
//      routines to set a set of pages as read-only or to unset them.
//      The read-only pages are for allocating user level code pages.
//
//      The code can be tested in user space by compiling with -D_TEST_MEM
//      and some additional debug statements are available using -D_DEBUG_MEM.
//      We can not test the low-level hardware interfacing code without being
//      on real hardware or in a simulator. This is in regards to the page
//      tables, the page directories, mem_size(), and page_enable.
//
// @notes:
//
// Develop a bitmap that is 4096 bytes long where
// each bit is a page.
//
// Develop code to initialize it at a fixed location.
// Develop code to allocate from it blocks.
// Develop code to free blocks.
// Develop code to set/unset a bit.
// 
// @NOTES:
// - KERNEL_END was supposed to be set by a loader script.
//   We instead set it to 2 MEG. Allowing the bottom 2 MEG
//   to be for the kernel.
//
#ifdef _TEST_MEM
#include <stdio.h>
#include "page.h"
#define printk printf
#else
#include <ox/mm/page.h>
#include <ox/mm/page_enable.h>
#include <ox/error_rpt.h>
#include <asm_core/io.h>
#include <platform/asm_core/util.h>
#endif

#ifndef __cplusplus
typedef int bool;
#define false 0
#define true  1
#endif

#define PAGE_BITS       32768 // 4096 * 8
#define BITS_PER_BYTE   8
#define _K_BASE         0x14000 // From s2.s in i386/boot (81920 in decimal)
#define ONE_MEG         (1<<20)
#define PAGE_TABLE_SIZE 1024 // 1024 entries in a page table four bytes each (PAGE_SIZE) in length.
// 1024 entries in a page table, 4096 bytes each == 4194304
// 1024 entries in page directory pointing to 1024 entries in page table, 4096 bytes each
// is 4GIG
#define ONE_FULL_PAGE_TABLE_BYTES 4194304 // Maximum number of bytes in a complete page table.

typedef struct mem_map {
    char page[PAGE_SIZE];
} mem_map_t;

#ifdef _TEST_MEM
#define MEMORY_SIZE (65536 * 4)
char memory[PAGE_SIZE * MEMORY_SIZE]; // This is for testing.
static mem_map_t *mem_map = (mem_map_t *)memory;
void asm_disable_interrupt() {}
void asm_enable_interrupt() {}
#else
static mem_map_t *mem_map = (mem_map_t *)0; // Address assigned in mem_init.
#endif

static   signed ALLOC_PAGES         = 0; // Number of pages allocated.
static   signed KALLOC_PAGES        = 0; // Number of kernel pages allocated.
static unsigned MEM_SIZE            = 0; // Size of memory in bytes.
static unsigned NR_PAGES            = 0; // Number of pages.
static unsigned NR_KPAGES           = 0; // Number of kernel pages (dynamic).
static unsigned NR_MEM_MAP          = 0; // Number of mem_maps need to represent.
static unsigned KLAST_PAGE          = 0; // Last page allocated/free'd in kernel.
static unsigned ULAST_PAGE          = 0; // Last page allocated/free'd in user.
static unsigned START_KMEM          = 0; // Start of kernel dynamic memory.
static unsigned START_UMEM          = 0; // Start of user dynamic memory.
static unsigned END_KMEM            = 0; // End of kernel dynamic memory.
static unsigned END_UMEM            = 0; // End of user dynamic memory.
static unsigned KBYTES_FREE         = 0; // Number of dynamic memory bytes available.
static unsigned UBYTES_FREE         = 0; // Number of user memory bytes available.
static unsigned NR_PAGE_TABLES      = 0; // Number of page tables needed.
static unsigned REAL_KERNEL_END     = 0; // KERNEL_END + _K_BASE;
static unsigned PAGE_TABLE_BYTES    = 0; // Sizeof page table NR_PAGE_TABLES * PAGE_SIZE.

#ifdef _TEST_MEM
static unsigned *KERNEL_PAGE_TABLE  = (unsigned *)0;
static unsigned *KERNEL_PAGE_DIR    = (unsigned *)memory;
unsigned KERNEL_END = (unsigned)((char *)memory + ONE_MEG);
unsigned char GDT_MAP[Nr_GDT];
#else
static unsigned *KERNEL_PAGE_TABLE  = (unsigned *)0x15000;
static unsigned *KERNEL_PAGE_DIR    = (unsigned *)0x14000;
static unsigned KERNEL_END          = 2 * ONE_MEG; // See notes above. Should be 0x200000.
unsigned char GDT_MAP[Nr_GDT]; // 1 if is in use, used to allocated gdt descriptors
#endif

void mem_set_bit(unsigned page);
void mem_clear_bit(unsigned page);
unsigned mem_test_bit(unsigned page);

//
// idpaging is derived from code
// from http://wiki.osdev.org/Identity_Paging
//
void idpaging(unsigned int *first_pte, unsigned int from, int size)
{
    from = from & 0xfffff000; // discard bits we don't want
    for(;size>0;from+=4096,size-=4096,first_pte++) {
        *first_pte=from | 1;     // mark page present.
    }
}

unsigned mem_get_size()
{
    return MEM_SIZE;
}

unsigned mem_size()
{
#ifdef _TEST_MEM
    return PAGE_SIZE * MEMORY_SIZE; // This is for testing.
#else
         register unsigned long *mem;
         unsigned long mem_count, a;
         unsigned short memkb;
         unsigned char irq1, irq2;
         register unsigned long cr0;

         /* save IRQ's */
         irq1=io_inb(0x21);
         irq2=io_inb(0xA1);

         /* kill all irq's */
         io_outb(0x21, 0xFF);
         io_outb(0xA1, 0xFF);

         mem_count=0;
         memkb=0;

         // store a copy of CR0
         __asm__ __volatile("movl %%cr0, %0":"=g"(cr0):);

         // invalidate the cache
         // write-back and invalidate the cache
         __asm__ __volatile__ ("wbinvd");

         // plug cr0 with just PE/CD/NW
         // cache disable(486+), no-writeback(486+), 32bit mode(386+)
         __asm__ __volatile__("movl %0, %%cr0" :: "g" (cr0 | 0x00000001 | 0x40000000 | 0x20000000));

         do
         {
                 memkb++;
                 mem_count+=ONE_MEG;
                 mem=(unsigned long *)mem_count;

                 a=*mem;

                 *mem=0x55AA55AA;
                 
                 // the empty asm calls tell gcc not to rely on whats in its registers
                 // as saved variables (this gets us around GCC optimisations)
                 asm("":::"memory");
                 if(*mem!=0x55AA55AA)
                         mem_count=0;
                 else
                 {
                         *mem=0xAA55AA55;
                         asm("":::"memory");
                         if(*mem!=0xAA55AA55)
                                 mem_count=0;
                 }

                 asm("":::"memory");
                 *mem=a;
         }while(memkb<4096 && mem_count!=0);

         __asm__ __volatile__("movl %0, %%cr0" :: "g" (cr0));

         // Obtain our calculated memory size.
         MEM_SIZE=memkb<<20;

         /*
          * TODO - The following is not currently used.
          * mem=(unsigned long *)0x413;
          * bse_end=((*mem)&0xFFFF)<<6;
          */

         io_outb(0x21, irq1);
         io_outb(0xA1, irq2);

         return MEM_SIZE;
#endif
}

void mem_init()
{
    register unsigned page = 0,bit = 0,i = 0,count = 0,page_limit = 0,virt = 0,frame = 0;
    register mem_map_t *mem;

    MEM_SIZE   = mem_size();
    NR_PAGES   = MEM_SIZE / PAGE_SIZE;
    NR_MEM_MAP = ((NR_PAGES / BITS_PER_BYTE) / PAGE_SIZE) + (((NR_PAGES / BITS_PER_BYTE) % PAGE_SIZE) ? 1 : 0);
    NR_PAGE_TABLES = NR_PAGES / PAGE_TABLE_SIZE;
    PAGE_TABLE_BYTES = NR_PAGE_TABLES * PAGE_SIZE;

    printk("OX Kernel memory parameters :=\n");
    printk("MEM_SIZE=%d\nNR_PAGES=%d\nNR_MEM_MAP=%d\nNR_PAGE_TABLES=%d\nPAGE_TABLE_BYTES=%d\n",
            MEM_SIZE, NR_PAGES, NR_MEM_MAP, NR_PAGE_TABLES, PAGE_TABLE_BYTES);

    // The memory map starts right where the kernel ends in RAM.
    // NOTE - The kernel doesn't start from 0 in RAM, as the lower region is reserved
    // so KERNEL_END must be offset based on where the boot loader places the kernel
    // in memory. We need to account for this here.
    // _K_BASE is at 0x14000 which the boot loader uses as the address
    // where the kernel is loaded. See the call in s2.s to exec_elf_kernel which passes
    // in as parameter %1 the value '_K_BASE' (assigned first to edx register). 
    // Then kernel_start is then the address of _start (based on `nm vmox`) which
    // is obtained when the exec_elf_kernel macro parses the ELF file and
    // loads it into memory. This is based on the -Ttext 0x100000 compiler/linker
    // option which is used to compile the kernel. It is from e_entry in the ELF
    // header. The kernel then is from 0x100000 + `size vmox` which we have
    // simplified above to be 2 * ONE_MEG giving a full one meg to the kernel
    // image in ram and KERNEL_END is 0x200000.
    //
    // I believe that we can simplify this by placing our memory
    // allocations at the start of the 2 MEG region, leaving
    // the bottom 2 MEG for the kernel and device addressing.
    // The kernel is still loaded at _K_BASE.
    // I believe the next important address is 0x90000 (leaving about 524288 bytes
    // for the kernel image as loaded from disk).
    // The actual image we run; however, is relocated in the exec_elf_kernel
    // macro to offset of 0x100000.

    // Off set this by KERNEL_END + sizeof
    // page table/directory for available ram. And assign the 
    // kernel page table/dir memory accordingly.
    REAL_KERNEL_END = KERNEL_END + ((NR_PAGES * 4) + PAGE_SIZE);
    KERNEL_PAGE_TABLE = (unsigned)KERNEL_END + PAGE_SIZE; /* Goes to NR_PAGES * 4 */
    KERNEL_PAGE_DIR = (unsigned)KERNEL_END; /* PAGE_SIZE directory */

    // Make sure we are page aligned.
    if(REAL_KERNEL_END % PAGE_SIZE) {
        REAL_KERNEL_END = (((REAL_KERNEL_END / PAGE_SIZE) + 1) * PAGE_SIZE);
    }
    // NR_MEM_MAP doesn't discount the pages needed for REAL_KERNEL_END.
    // It should map directly to memory that is available.
    mem_map = (mem_map_t *)(PAGE_TABLE_BYTES + REAL_KERNEL_END);
    // Calculate START_KMEM as KERNEL_END + (NR_MEM_MAP * sizeof(mem_map_t)).
    START_KMEM = PAGE_TABLE_BYTES + REAL_KERNEL_END + (NR_MEM_MAP * sizeof(mem_map_t));

    printk("mem_map=%d\nSTART_KMEM=%d\nREAL_KERNEL_END=%d\nKERNEL_END=%d\n_K_BASE=%d\n",
            (unsigned)mem_map,START_KMEM,REAL_KERNEL_END,KERNEL_END,_K_BASE);
    // START_KMEM must be on a PAGE_SIZE boundary as it is used
    // as the base for allocated RAM.
    if(START_KMEM % PAGE_SIZE) {
        START_KMEM = (((START_KMEM / PAGE_SIZE) + 1) * PAGE_SIZE);
    }

    // Give one ONE_FULL_PAGE_TABLE_BYTES to the kernel for dynamic memory.
    // The user gets the rest.
    END_KMEM    = 4 * ONE_FULL_PAGE_TABLE_BYTES;
    while(END_KMEM <= START_KMEM) {
        END_KMEM += ONE_FULL_PAGE_TABLE_BYTES; // Assert our kernel memory actually ends after start.
    }
    START_UMEM  = END_KMEM;
    END_UMEM    = MEM_SIZE;
    // Calculate how many bytes available.
    KBYTES_FREE = END_KMEM - START_KMEM;
    UBYTES_FREE = END_UMEM - START_UMEM;
    NR_KPAGES = END_KMEM / PAGE_SIZE;

    printk("START_KMEM=%d\nEND_KMEM=%d\nSTART_UMEM=%d\nEND_UMEM=%d\nKBYTES_FREE=%d\nUBYTES_FREE=%d\nNR_KPAGES=%d\n",
            START_KMEM,END_KMEM,START_UMEM,END_UMEM,KBYTES_FREE,UBYTES_FREE,NR_KPAGES);

    printk("initializing memory bitmaps\n");
    // Initialize bitmaps.
    for(page = 0; page < NR_MEM_MAP; ++page) {
        mem = &mem_map[page];
        for(bit = 0; bit < PAGE_SIZE; ++bit) {
            mem->page[bit] = false;
        }
    }

#ifdef _ENABLE_PAGING
    printk("loading KERNEL_PAGE_TABLEs\n");

    page_limit = END_KMEM / PAGE_SIZE;
    printk(" page_limit [%d]\n",page_limit);
    for(i=0,frame=0,virt=0; i < NR_PAGES; ++i,frame += PAGE_SIZE,virt += PAGE_SIZE) {
        if(i < page_limit) {
            // Kernel page tables.
            page = 0;
            PT_SET_ATTRIB(page, PTE_PRESENT);
            PT_SET_ATTRIB(page, PTE_READ_WRITE);
            PT_SET_FRAME(page, frame);
            KERNEL_PAGE_TABLE[i] = page;
        } else {
            page = 0;
            PT_SET_ATTRIB(page, PTE_PRESENT);
            PT_SET_ATTRIB(page, PTE_READ_WRITE);
            PT_SET_ATTRIB(page, PTE_USER);
            PT_SET_FRAME(page, frame);
            KERNEL_PAGE_TABLE[i] = page;
        }
    }

    printk("loading KERNEL_PAGE_DIR NR_PAGE_TABLES\n");
    // Write starting at address 0 which is where the kernel page dir
    // resides (see page_enable.s).
    page_limit = END_KMEM / ONE_FULL_PAGE_TABLE_BYTES;
    printk(" page_limit [%d]\n",page_limit);
    for(i = 0, frame = (unsigned)KERNEL_PAGE_TABLE; 
            i < NR_PAGE_TABLES; ++i, frame += PAGE_SIZE) {
        if(i < page_limit) {
            page = 0;
            PD_SET_ATTRIB(page, PDE_PRESENT);
            PD_SET_ATTRIB(page, PDE_READ_WRITE);
            PD_SET_FRAME(page, frame);
            KERNEL_PAGE_DIR[i] = page;
        } else {
            page = 0;
            PD_SET_ATTRIB(page, PDE_PRESENT);
            PD_SET_ATTRIB(page, PDE_READ_WRITE);
            PD_SET_ATTRIB(page, PDE_USER);
            PD_SET_FRAME(page, frame);
            KERNEL_PAGE_DIR[i] = page;
        }
    }

    printk("NR_PAGE_TABLES %d\n",NR_PAGE_TABLES); // 64 1024 * 4096 page tables == 256 meg
    printk("KERNEL_PAGE_TABLE %d\n",KERNEL_PAGE_TABLE);
    printk("KERNEL_PAGE_TABLE[NR_PAGES-1] %u\n",KERNEL_PAGE_TABLE[NR_PAGES-1]);
    printk("KERNEL_PAGE_TABLE[4097] %u\n",KERNEL_PAGE_TABLE[4097]);
    printk("KERNEL_PAGE_DIR [%d]\n",KERNEL_PAGE_DIR);
    printk("KERNEL_PAGE_DIR[0] %d\n",KERNEL_PAGE_DIR[0]);

#endif

    /*
     * The following code can be used to test 
     * identity paging the lower 4 megs of RAM.
     * 
     *  idpaging(KERNEL_PAGE_TABLE, 0x0, 4 * ONE_MEG);
     *  KERNEL_PAGE_DIR[0] = ((unsigned)KERNEL_PAGE_TABLE) | 1;
     *
     */
    // Mark the kernel pages where the kernel has been loaded as being used.
    // These are pages from 0 to START_KMEM / PAGE_SIZE as START_KMEM is start
    // of kernel dynamic memory and all things below are pre-allocated.
    printk("reserving kernel loaded memory bitmaps\n");
    page = START_KMEM / PAGE_SIZE;
    for(i = 0; i < page; ++i) {
        mem_set_bit(i);
    }
    // Now in the kpage_alloc and page_alloc we figure out our region
    // start using START_KMEM / PAGE_SIZE and START_UMEM / PAGE_SIZE
    // and we return addresses as PAGE_SIZE * i.
#ifdef _ENABLE_PAGING
    printk("enabling paging\n");
    page_enable(KERNEL_PAGE_DIR);
    printk("done enabling paging\n");
#elif _NO_PAGING
    printk("paging not enabled\n");
#endif
 
    printk("initializing GDT_MAP\n");
    // GDT_MAP marks which GDTs are in use.
    for(i=0; i < Nr_GDT; ++i)
        GDT_MAP[i]=false;

    for(i=0; i < 6; ++i)
        GDT_MAP[i]=true;

    printk("done initializing memory\n");
}

unsigned get_nr_pages()
{
    return NR_PAGES;
}

unsigned get_nr_kpages()
{
    return NR_KPAGES;
}

unsigned get_nr_allocated_pages()
{
    return ALLOC_PAGES;
}

unsigned get_nr_kallocated_pages()
{
    return KALLOC_PAGES;
}

unsigned get_user_bytes_free()
{
    return UBYTES_FREE - (ALLOC_PAGES * PAGE_SIZE);
}

unsigned get_kernel_bytes_free()
{
    return KBYTES_FREE - (KALLOC_PAGES * PAGE_SIZE);
}

// These are needed to set user pages as read only
// for use in code segments, otherwise, the user
// process would be allowed to modify its text region
// at run time.
void mem_set_read_only(void *addr, unsigned nr_pages)
{
    unsigned page_start = (((unsigned)(char *)addr) / PAGE_SIZE);
    unsigned i = 0, page = 0;
    asm_disable_interrupt();
    if(nr_pages > NR_PAGES) {
        printk("mem_set_read_only:: warning nr_pages > NR_PAGES setting to NR_PAGES\n");
        nr_pages = NR_PAGES; 
    }
    for(i = page_start; i < (page_start + nr_pages); ++i) {
        page = KERNEL_PAGE_TABLE[i];
        // Clear the second bit which makes this 
        // page read-only.
        PT_SET_READ_ONLY(page);
        KERNEL_PAGE_TABLE[i] = page;
        page_flush_tlb(i * PAGE_SIZE);
    }
    asm_enable_interrupt();
}

void mem_unset_read_only(void *addr, unsigned nr_pages)
{
    unsigned page_start = (((unsigned)(char *)addr) / PAGE_SIZE);
    unsigned i = 0, page = 0;
    asm_disable_interrupt();
    if(nr_pages > NR_PAGES) {
        printk("mem_set_read_only:: warning nr_pages > NR_PAGES setting to NR_PAGES\n");
        nr_pages = NR_PAGES; 
    }
    for(i = page_start; i < (page_start + nr_pages); ++i) {
        page = KERNEL_PAGE_TABLE[i];
        PT_SET_ATTRIB(page, PTE_READ_WRITE);
        KERNEL_PAGE_TABLE[i] = page;
        page_flush_tlb(i * PAGE_SIZE);
    }
    asm_enable_interrupt();
}

void mem_set_bit(unsigned page)
{
    register unsigned block = page / PAGE_BITS;
    register unsigned byte  = page % PAGE_BITS / BITS_PER_BYTE;
    register unsigned bit   = page % PAGE_BITS % BITS_PER_BYTE;
    register mem_map_t *mem = &mem_map[block];
#ifdef _DEBUG_MEM
    printf("block=%d\tbyte=%d\tbit=%d\t\n",block,byte,bit);
#endif
    mem->page[byte] |= (1 << bit);
#ifdef _DEBUG_MEM
    printf("mem->page[byte]=%d\n",mem->page[byte]);
#endif
}

void mem_clear_bit(unsigned page)
{
    register unsigned block = page / PAGE_BITS;
    register unsigned byte  = page % PAGE_BITS / BITS_PER_BYTE;
    register unsigned bit   = page % PAGE_BITS % BITS_PER_BYTE;
    register mem_map_t *mem = &mem_map[block];
#ifdef _DEBUG_MEM
    printf("block=%d\tbyte=%d\tbit=%d\t\n",block,byte,bit);
#endif
    mem->page[byte] &= ~(1 << bit);
}

unsigned mem_test_bit(unsigned page)
{
    register unsigned block = page / PAGE_BITS;
    register unsigned byte  = page % PAGE_BITS / BITS_PER_BYTE;
    register unsigned bit   = page % PAGE_BITS % BITS_PER_BYTE;
    register mem_map_t *mem = &mem_map[block];
#ifdef _DEBUG_MEM
    printf("block=%d\tbyte=%d\tbit=%d\t\n",block,byte,bit);
    printf("(mem->page[byte] & (1 << bit) >> bit)=%d\n",((mem->page[byte] & (1 << bit)) >> bit));
#endif
    return ((mem->page[byte] & (1 << bit)) >> bit);
}

void *page_alloc(unsigned nr_pages)
{
    // Use two loops, scan for a region as big as nr_pages.
    // Return as START_MEM + (page_start * PAGE_SIZE)
    // Less what we have for a header indicating the size.
    register unsigned page = ULAST_PAGE;
    register unsigned run  = 0;

    if((ALLOC_PAGES * PAGE_SIZE) >= UBYTES_FREE) {
        return (void *)0;
    }

    asm_disable_interrupt();
    if(ULAST_PAGE == 0) {
        ULAST_PAGE = START_UMEM / PAGE_SIZE;
    }
    // Scan from last page allocated/free'd.
    for(page = ULAST_PAGE; page < NR_PAGES; ++page) {
        if(mem_test_bit(page) == false) {
            // Found a possible run.
            for(run = 0; run < nr_pages; ++run) {
                if(mem_test_bit(page + run) == true) {
                    break;
                }
            }
            if(run == nr_pages) {
                // Found a run.
                for(run = 0; run < nr_pages; ++run) {
                    mem_set_bit(page + run);
                }
                // Next scan is past what we allocated.
                ULAST_PAGE = page + run + 1;
                // printk("ULAST_PAGE [%d]\n",ULAST_PAGE); // RGDDEBUG
                ALLOC_PAGES += nr_pages;
                asm_enable_interrupt();
                return (void *)(page * PAGE_SIZE);
            } else {
                // Skip past the ones we know don't fit.
                page += run;
            }
        }
    }
    // Didn't find any sufficient runs, scan from 0 to
    // ULAST_PAGE.
    for(page = START_UMEM / PAGE_SIZE; page < ULAST_PAGE; ++page) {
        if(mem_test_bit(page) == false) {
            // Found a possible run.
            for(run = 0; run < nr_pages; ++run) {
                if(mem_test_bit(page + run) == true) {
                    break;
                }
            }
            if(run == nr_pages) {
                // Found a run.
                for(run = 0; run < nr_pages; ++run) {
                    mem_set_bit(page + run);
                }
                // Next scan is past what we allocated.
                ULAST_PAGE = page + run + 1;
                ALLOC_PAGES += nr_pages;
                asm_enable_interrupt();
                // printk("ULAST_PAGE [%d]\n",ULAST_PAGE); // RGDDEBUG
                return (void *)(page * PAGE_SIZE);
            } else {
                // Skip past the ones we know don't fit.
                page += run;
            }
        }
    }
    // Not enough memory.
    asm_enable_interrupt();
    return (void *)0;
}

void page_free(void *addr, unsigned nr_pages)
{
    // Subtract our header, get our size
    // Calculate our page by subtracting START_MEM and then dividing by PAGE_SIZE.
    // Go through the whole run and set bits to 0.
    // *Note* we should store the number of pages requested in malloc.c
    // as its easier to manage there instead of here so review that code.
    register unsigned page = ((unsigned)(char *)addr / PAGE_SIZE);
    register unsigned run  = 0;
    if(page < NR_KPAGES || page > NR_PAGES) {
        panic("page_free:: free'ing invalid page [%d] NR_KPAGES [%d] NR_PAGES [%d]\n",page,NR_KPAGES,NR_PAGES);
    }
    asm_disable_interrupt();
    for(run = 0; run < nr_pages; ++run) {
        mem_clear_bit(page + run);
    }
    ULAST_PAGE = page;
    ALLOC_PAGES -= nr_pages;
    if(ALLOC_PAGES < 0) {
        panic("page_free:: error too many pages free'd\n");
    }
    // printk("ULAST_PAGE [%d]\n",ULAST_PAGE); // RGDDEBUG
    asm_enable_interrupt();
}

void *kpage_alloc(unsigned nr_pages)
{
    // Use two loops, scan for a region as big as nr_pages.
    // Return as START_MEM + (page_start * PAGE_SIZE)
    // Less what we have for a header indicating the size.
    register unsigned page = ULAST_PAGE;
    register unsigned run  = 0;

    if((KALLOC_PAGES * PAGE_SIZE) >= KBYTES_FREE) {
        return (void *)0;
    }

    asm_disable_interrupt();
    if(KLAST_PAGE == 0) {
        KLAST_PAGE = START_KMEM / PAGE_SIZE;
    }
    // Scan from last page allocated/free'd.
    for(page = KLAST_PAGE; page < NR_KPAGES; ++page) {
        if(mem_test_bit(page) == false) {
            // Found a possible run.
            for(run = 0; run < nr_pages; ++run) {
                if(mem_test_bit(page + run) == true) {
                    break;
                }
            }
            if(run == nr_pages) {
                // Found a run.
                for(run = 0; run < nr_pages; ++run) {
                    mem_set_bit(page + run);
                }
                // Next scan is past what we allocated.
                KLAST_PAGE = page + run + 1;
                // printk("KLAST_PAGE [%d]\n",KLAST_PAGE); // RGDDEBUG
                KALLOC_PAGES += nr_pages;
                asm_enable_interrupt();
                return (void *)(page * PAGE_SIZE);
            } else {
                // Skip past the ones we know don't fit.
                page += run;
            }
        }
    }
    // Didn't find any sufficient runs, scan from 0 to
    // KLAST_PAGE.
    for(page = START_KMEM / PAGE_SIZE; page < KLAST_PAGE; ++page) {
        if(mem_test_bit(page) == false) {
            // Found a possible run.
            for(run = 0; run < nr_pages; ++run) {
                if(mem_test_bit(page + run) == true) {
                    break;
                }
            }
            if(run == nr_pages) {
                // Found a run.
                for(run = 0; run < nr_pages; ++run) {
                    mem_set_bit(page + run);
                }
                // Next scan is past what we allocated.
                KLAST_PAGE = page + run + 1;
                // printk("KLAST_PAGE [%d]\n",KLAST_PAGE); // RGDDEBUG
                KALLOC_PAGES += nr_pages;
                asm_enable_interrupt();
                return (void *)(page * PAGE_SIZE);
            } else {
                // Skip past the ones we know don't fit.
                page += run;
            }
        }
    }
    // Not enough memory.
    asm_enable_interrupt();
    return (void *)0;
}

void kpage_free(void *addr, unsigned nr_pages)
{
    // Subtract our header, get our size
    // Calculate our page by subtracting START_MEM and then dividing by PAGE_SIZE.
    // Go through the whole run and set bits to 0.
    // *Note* we should store the number of pages requested in malloc.c
    // as its easier to manage there instead of here so review that code.
    register unsigned page = ((unsigned)(char *)addr / PAGE_SIZE);
    register unsigned run  = 0;
    if(page > NR_KPAGES) {
        panic("kpage_free:: free'ing invalid page [%d] NR_KPAGES [%d]\n",page,NR_KPAGES);
    }
    asm_disable_interrupt();
    for(run = 0; run < nr_pages; ++run) {
        mem_clear_bit(page + run);
    }
    KLAST_PAGE = page;
    KALLOC_PAGES -= nr_pages;
    if(KALLOC_PAGES < 0) {
        panic("kpage_free:: error too many pages free'd\n");
    }
    // printk("KLAST_PAGE [%d]\n",KLAST_PAGE); // RGDDEBUG
    asm_enable_interrupt();
}

int alloc_gdt()
{
    int i = 0;
    for(i = 0; i < Nr_GDT; ++i) {
        // First 6 are assigned to kernel, see mm/page.c
        if(!GDT_MAP[i]) {
            GDT_MAP[i]=1;
            return i;
        }
    }
    panic("alloc_gdt:: no free gdt descriptors\n");
    return -1;
}// alloc_gdt

void setup_paging()
{
    page_enable(KERNEL_PAGE_DIR);
}// setup_paging

unsigned *get_page_dir()
{
    // Should be setup in cr3 in tss on start
    // of a new task.
    return KERNEL_PAGE_DIR;
}// get_page_dir

#ifdef _TEST_MEM
int
main()
{
    mem_init();
    mem_set_bit(0);
    if(mem_test_bit(0) == true) {
        printf("successfully set page bit 0\n");
    }
    mem_clear_bit(0);
    if(mem_test_bit(0) == false) {
        printf("successfully cleared page bit 0\n");
    }
    mem_set_bit(32768 + 10);
    if(mem_test_bit(32768 + 10) == true) {
        printf("successfully set page bit 32768 + 10\n");
    } else {
        printf("error\n");
    }
    mem_set_bit(32768 + 11);
    if(mem_test_bit(32768 + 11) == true) {
        printf("successfully set page bit 32768 + 11\n");
    } else {
        printf("error\n");
    }
    mem_clear_bit(32768 + 10);
    if(mem_test_bit(32768 + 10) == false) {
        printf("successfully cleared page bit 32768 + 10\n");
    }
}
#endif

