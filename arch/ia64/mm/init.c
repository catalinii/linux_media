// SPDX-License-Identifier: GPL-2.0
/*
 * Initialize MMU support.
 *
 * Copyright (C) 1998-2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 */
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/dma-map-ops.h>
#include <linux/dmar.h>
#include <linux/efi.h>
#include <linux/elf.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/personality.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/proc_fs.h>
#include <linux/bitops.h>
#include <linux/kexec.h>
#include <linux/swiotlb.h>

#include <asm/dma.h>
#include <asm/io.h>
#include <asm/numa.h>
#include <asm/patch.h>
#include <asm/pgalloc.h>
#include <asm/sal.h>
#include <asm/sections.h>
#include <asm/tlb.h>
#include <linux/uaccess.h>
#include <asm/unistd.h>
#include <asm/mca.h>

extern void ia64_tlb_init (void);

unsigned long MAX_DMA_ADDRESS = PAGE_OFFSET + 0x100000000UL;

#ifdef CONFIG_VIRTUAL_MEM_MAP
unsigned long VMALLOC_END = VMALLOC_END_INIT;
EXPORT_SYMBOL(VMALLOC_END);
struct page *vmem_map;
EXPORT_SYMBOL(vmem_map);
#endif

struct page *zero_page_memmap_ptr;	/* map entry for zero page */
EXPORT_SYMBOL(zero_page_memmap_ptr);

void
__ia64_sync_icache_dcache (pte_t pte)
{
	unsigned long addr;
	struct page *page;

	page = pte_page(pte);
	addr = (unsigned long) page_address(page);

	if (test_bit(PG_arch_1, &page->flags))
		return;				/* i-cache is already coherent with d-cache */

	flush_icache_range(addr, addr + page_size(page));
	set_bit(PG_arch_1, &page->flags);	/* mark page as clean */
}

/*
 * Since DMA is i-cache coherent, any (complete) pages that were written via
 * DMA can be marked as "clean" so that lazy_mmu_prot_update() doesn't have to
 * flush them when they get mapped into an executable vm-area.
 */
void arch_dma_mark_clean(phys_addr_t paddr, size_t size)
{
	unsigned long pfn = PHYS_PFN(paddr);

	do {
		set_bit(PG_arch_1, &pfn_to_page(pfn)->flags);
	} while (++pfn <= PHYS_PFN(paddr + size - 1));
}

inline void
ia64_set_rbs_bot (void)
{
	unsigned long stack_size = rlimit_max(RLIMIT_STACK) & -16;

	if (stack_size > MAX_USER_STACK_SIZE)
		stack_size = MAX_USER_STACK_SIZE;
	current->thread.rbs_bot = PAGE_ALIGN(current->mm->start_stack - stack_size);
}

/*
 * This performs some platform-dependent address space initialization.
 * On IA-64, we want to setup the VM area for the register backing
 * store (which grows upwards) and install the gateway page which is
 * used for signal trampolines, etc.
 */
void
ia64_init_addr_space (void)
{
	struct vm_area_struct *vma;

	ia64_set_rbs_bot();

	/*
	 * If we're out of memory and kmem_cache_alloc() returns NULL, we simply ignore
	 * the problem.  When the process attempts to write to the register backing store
	 * for the first time, it will get a SEGFAULT in this case.
	 */
	vma = vm_area_alloc(current->mm);
	if (vma) {
		vma_set_anonymous(vma);
		vma->vm_start = current->thread.rbs_bot & PAGE_MASK;
		vma->vm_end = vma->vm_start + PAGE_SIZE;
		vma->vm_flags = VM_DATA_DEFAULT_FLAGS|VM_GROWSUP|VM_ACCOUNT;
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
		mmap_write_lock(current->mm);
		if (insert_vm_struct(current->mm, vma)) {
			mmap_write_unlock(current->mm);
			vm_area_free(vma);
			return;
		}
		mmap_write_unlock(current->mm);
	}

	/* map NaT-page at address zero to speed up speculative dereferencing of NULL: */
	if (!(current->personality & MMAP_PAGE_ZERO)) {
		vma = vm_area_alloc(current->mm);
		if (vma) {
			vma_set_anonymous(vma);
			vma->vm_end = PAGE_SIZE;
			vma->vm_page_prot = __pgprot(pgprot_val(PAGE_READONLY) | _PAGE_MA_NAT);
			vma->vm_flags = VM_READ | VM_MAYREAD | VM_IO |
					VM_DONTEXPAND | VM_DONTDUMP;
			mmap_write_lock(current->mm);
			if (insert_vm_struct(current->mm, vma)) {
				mmap_write_unlock(current->mm);
				vm_area_free(vma);
				return;
			}
			mmap_write_unlock(current->mm);
		}
	}
}

void
free_initmem (void)
{
	free_reserved_area(ia64_imva(__init_begin), ia64_imva(__init_end),
			   -1, "unused kernel");
}

void __init
free_initrd_mem (unsigned long start, unsigned long end)
{
	/*
	 * EFI uses 4KB pages while the kernel can use 4KB or bigger.
	 * Thus EFI and the kernel may have different page sizes. It is
	 * therefore possible to have the initrd share the same page as
	 * the end of the kernel (given current setup).
	 *
	 * To avoid freeing/using the wrong page (kernel sized) we:
	 *	- align up the beginning of initrd
	 *	- align down the end of initrd
	 *
	 *  |             |
	 *  |=============| a000
	 *  |             |
	 *  |             |
	 *  |             | 9000
	 *  |/////////////|
	 *  |/////////////|
	 *  |=============| 8000
	 *  |///INITRD////|
	 *  |/////////////|
	 *  |/////////////| 7000
	 *  |             |
	 *  |KKKKKKKKKKKKK|
	 *  |=============| 6000
	 *  |KKKKKKKKKKKKK|
	 *  |KKKKKKKKKKKKK|
	 *  K=kernel using 8KB pages
	 *
	 * In this example, we must free page 8000 ONLY. So we must align up
	 * initrd_start and keep initrd_end as is.
	 */
	start = PAGE_ALIGN(start);
	end = end & PAGE_MASK;

	if (start < end)
		printk(KERN_INFO "Freeing initrd memory: %ldkB freed\n", (end - start) >> 10);

	for (; start < end; start += PAGE_SIZE) {
		if (!virt_addr_valid(start))
			continue;
		free_reserved_page(virt_to_page(start));
	}
}

/*
 * This installs a clean page in the kernel's page table.
 */
static struct page * __init
put_kernel_page (struct page *page, unsigned long address, pgprot_t pgprot)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset_k(address);		/* note: this is NOT pgd_offset()! */

	{
		p4d = p4d_alloc(&init_mm, pgd, address);
		if (!p4d)
			goto out;
		pud = pud_alloc(&init_mm, p4d, address);
		if (!pud)
			goto out;
		pmd = pmd_alloc(&init_mm, pud, address);
		if (!pmd)
			goto out;
		pte = pte_alloc_kernel(pmd, address);
		if (!pte)
			goto out;
		if (!pte_none(*pte))
			goto out;
		set_pte(pte, mk_pte(page, pgprot));
	}
  out:
	/* no need for flush_tlb */
	return page;
}

static void __init
setup_gate (void)
{
	struct page *page;

	/*
	 * Map the gate page twice: once read-only to export the ELF
	 * headers etc. and once execute-only page to enable
	 * privilege-promotion via "epc":
	 */
	page = virt_to_page(ia64_imva(__start_gate_section));
	put_kernel_page(page, GATE_ADDR, PAGE_READONLY);
#ifdef HAVE_BUGGY_SEGREL
	page = virt_to_page(ia64_imva(__start_gate_section + PAGE_SIZE));
	put_kernel_page(page, GATE_ADDR + PAGE_SIZE, PAGE_GATE);
#else
	put_kernel_page(page, GATE_ADDR + PERCPU_PAGE_SIZE, PAGE_GATE);
	/* Fill in the holes (if any) with read-only zero pages: */
	{
		unsigned long addr;

		for (addr = GATE_ADDR + PAGE_SIZE;
		     addr < GATE_ADDR + PERCPU_PAGE_SIZE;
		     addr += PAGE_SIZE)
		{
			put_kernel_page(ZERO_PAGE(0), addr,
					PAGE_READONLY);
			put_kernel_page(ZERO_PAGE(0), addr + PERCPU_PAGE_SIZE,
					PAGE_READONLY);
		}
	}
#endif
	ia64_patch_gate();
}

static struct vm_area_struct gate_vma;

static int __init gate_vma_init(void)
{
	vma_init(&gate_vma, NULL);
	gate_vma.vm_start = FIXADDR_USER_START;
	gate_vma.vm_end = FIXADDR_USER_END;
	gate_vma.vm_flags = VM_READ | VM_MAYREAD | VM_EXEC | VM_MAYEXEC;
	gate_vma.vm_page_prot = __P101;

	return 0;
}
__initcall(gate_vma_init);

struct vm_area_struct *get_gate_vma(struct mm_struct *mm)
{
	return &gate_vma;
}

int in_gate_area_no_mm(unsigned long addr)
{
	if ((addr >= FIXADDR_USER_START) && (addr < FIXADDR_USER_END))
		return 1;
	return 0;
}

int in_gate_area(struct mm_struct *mm, unsigned long addr)
{
	return in_gate_area_no_mm(addr);
}

void ia64_mmu_init(void *my_cpu_data)
{
	unsigned long pta, impl_va_bits;
	extern void tlb_init(void);

#ifdef CONFIG_DISABLE_VHPT
#	define VHPT_ENABLE_BIT	0
#else
#	define VHPT_ENABLE_BIT	1
#endif

	/*
	 * Check if the virtually mapped linear page table (VMLPT) overlaps with a mapped
	 * address space.  The IA-64 architecture guarantees that at least 50 bits of
	 * virtual address space are implemented but if we pick a large enough page size
	 * (e.g., 64KB), the mapped address space is big enough that it will overlap with
	 * VMLPT.  I assume that once we run on machines big enough to warrant 64KB pages,
	 * IMPL_VA_MSB will be significantly bigger, so this is unlikely to become a
	 * problem in practice.  Alternatively, we could truncate the top of the mapped
	 * address space to not permit mappings that would overlap with the VMLPT.
	 * --davidm 00/12/06
	 */
#	define pte_bits			3
#	define mapped_space_bits	(3*(PAGE_SHIFT - pte_bits) + PAGE_SHIFT)
	/*
	 * The virtual page table has to cover the entire implemented address space within
	 * a region even though not all of this space may be mappable.  The reason for
	 * this is that the Access bit and Dirty bit fault handlers perform
	 * non-speculative accesses to the virtual page table, so the address range of the
	 * virtual page table itself needs to be covered by virtual page table.
	 */
#	define vmlpt_bits		(impl_va_bits - PAGE_SHIFT + pte_bits)
#	define POW2(n)			(1ULL << (n))

	impl_va_bits = ffz(~(local_cpu_data->unimpl_va_mask | (7UL << 61)));

	if (impl_va_bits < 51 || impl_va_bits > 61)
		panic("CPU has bogus IMPL_VA_MSB value of %lu!\n", impl_va_bits - 1);
	/*
	 * mapped_space_bits - PAGE_SHIFT is the total number of ptes we need,
	 * which must fit into "vmlpt_bits - pte_bits" slots. Second half of
	 * the test makes sure that our mapped space doesn't overlap the
	 * unimplemented hole in the middle of the region.
	 */
	if ((mapped_space_bits - PAGE_SHIFT > vmlpt_bits - pte_bits) ||
	    (mapped_space_bits > impl_va_bits - 1))
		panic("Cannot build a big enough virtual-linear page table"
		      " to cover mapped address space.\n"
		      " Try using a smaller page size.\n");


	/* place the VMLPT at the end of each page-table mapped region: */
	pta = POW2(61) - POW2(vmlpt_bits);

	/*
	 * Set the (virtually mapped linear) page table address.  Bit
	 * 8 selects between the short and long format, bits 2-7 the
	 * size of the table, and bit 0 whether the VHPT walker is
	 * enabled.
	 */
	ia64_set_pta(pta | (0 << 8) | (vmlpt_bits << 2) | VHPT_ENABLE_BIT);

	ia64_tlb_init();

#ifdef	CONFIG_HUGETLB_PAGE
	ia64_set_rr(HPAGE_REGION_BASE, HPAGE_SHIFT << 2);
	ia64_srlz_d();
#endif
}

#ifdef CONFIG_VIRTUAL_MEM_MAP
int vmemmap_find_next_valid_pfn(int node, int i)
{
	unsigned long end_address, hole_next_pfn;
	unsigned long stop_address;
	pg_data_t *pgdat = NODE_DATA(node);

	end_address = (unsigned long) &vmem_map[pgdat->node_start_pfn + i];
	end_address = PAGE_ALIGN(end_address);
	stop_address = (unsigned long) &vmem_map[pgdat_end_pfn(pgdat)];

	do {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;

		pgd = pgd_offset_k(end_address);
		if (pgd_none(*pgd)) {
			end_address += PGDIR_SIZE;
			continue;
		}

		p4d = p4d_offset(pgd, end_address);
		if (p4d_none(*p4d)) {
			end_address += P4D_SIZE;
			continue;
		}

		pud = pud_offset(p4d, end_address);
		if (pud_none(*pud)) {
			end_address += PUD_SIZE;
			continue;
		}

		pmd = pmd_offset(pud, end_address);
		if (pmd_none(*pmd)) {
			end_address += PMD_SIZE;
			continue;
		}

		pte = pte_offset_kernel(pmd, end_address);
retry_pte:
		if (pte_none(*pte)) {
			end_address += PAGE_SIZE;
			pte++;
			if ((end_address < stop_address) &&
			    (end_address != ALIGN(end_address, 1UL << PMD_SHIFT)))
				goto retry_pte;
			continue;
		}
		/* Found next valid vmem_map page */
		break;
	} while (end_address < stop_address);

	end_address = min(end_address, stop_address);
	end_address = end_address - (unsigned long) vmem_map + sizeof(struct page) - 1;
	hole_next_pfn = end_address / sizeof(struct page);
	return hole_next_pfn - pgdat->node_start_pfn;
}

int __init create_mem_map_page_table(u64 start, u64 end, void *arg)
{
	unsigned long address, start_page, end_page;
	struct page *map_start, *map_end;
	int node;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	map_start = vmem_map + (__pa(start) >> PAGE_SHIFT);
	map_end   = vmem_map + (__pa(end) >> PAGE_SHIFT);

	start_page = (unsigned long) map_start & PAGE_MASK;
	end_page = PAGE_ALIGN((unsigned long) map_end);
	node = paddr_to_nid(__pa(start));

	for (address = start_page; address < end_page; address += PAGE_SIZE) {
		pgd = pgd_offset_k(address);
		if (pgd_none(*pgd)) {
			p4d = memblock_alloc_node(PAGE_SIZE, PAGE_SIZE, node);
			if (!p4d)
				goto err_alloc;
			pgd_populate(&init_mm, pgd, p4d);
		}
		p4d = p4d_offset(pgd, address);

		if (p4d_none(*p4d)) {
			pud = memblock_alloc_node(PAGE_SIZE, PAGE_SIZE, node);
			if (!pud)
				goto err_alloc;
			p4d_populate(&init_mm, p4d, pud);
		}
		pud = pud_offset(p4d, address);

		if (pud_none(*pud)) {
			pmd = memblock_alloc_node(PAGE_SIZE, PAGE_SIZE, node);
			if (!pmd)
				goto err_alloc;
			pud_populate(&init_mm, pud, pmd);
		}
		pmd = pmd_offset(pud, address);

		if (pmd_none(*pmd)) {
			pte = memblock_alloc_node(PAGE_SIZE, PAGE_SIZE, node);
			if (!pte)
				goto err_alloc;
			pmd_populate_kernel(&init_mm, pmd, pte);
		}
		pte = pte_offset_kernel(pmd, address);

		if (pte_none(*pte)) {
			void *page = memblock_alloc_node(PAGE_SIZE, PAGE_SIZE,
							 node);
			if (!page)
				goto err_alloc;
			set_pte(pte, pfn_pte(__pa(page) >> PAGE_SHIFT,
					     PAGE_KERNEL));
		}
	}
	return 0;

err_alloc:
	panic("%s: Failed to allocate %lu bytes align=0x%lx nid=%d\n",
	      __func__, PAGE_SIZE, PAGE_SIZE, node);
	return -ENOMEM;
}

struct memmap_init_callback_data {
	struct page *start;
	struct page *end;
	int nid;
	unsigned long zone;
};

static int __meminit
virtual_memmap_init(u64 start, u64 end, void *arg)
{
	struct memmap_init_callback_data *args;
	struct page *map_start, *map_end;

	args = (struct memmap_init_callback_data *) arg;
	map_start = vmem_map + (__pa(start) >> PAGE_SHIFT);
	map_end   = vmem_map + (__pa(end) >> PAGE_SHIFT);

	if (map_start < args->start)
		map_start = args->start;
	if (map_end > args->end)
		map_end = args->end;

	/*
	 * We have to initialize "out of bounds" struct page elements that fit completely
	 * on the same pages that were allocated for the "in bounds" elements because they
	 * may be referenced later (and found to be "reserved").
	 */
	map_start -= ((unsigned long) map_start & (PAGE_SIZE - 1)) / sizeof(struct page);
	map_end += ((PAGE_ALIGN((unsigned long) map_end) - (unsigned long) map_end)
		    / sizeof(struct page));

	if (map_start < map_end)
		memmap_init_zone((unsigned long)(map_end - map_start),
				 args->nid, args->zone, page_to_pfn(map_start),
				 MEMINIT_EARLY, NULL, MIGRATE_MOVABLE);
	return 0;
}

void __meminit
memmap_init (unsigned long size, int nid, unsigned long zone,
	     unsigned long start_pfn)
{
	if (!vmem_map) {
		memmap_init_zone(size, nid, zone, start_pfn,
				 MEMINIT_EARLY, NULL, MIGRATE_MOVABLE);
	} else {
		struct page *start;
		struct memmap_init_callback_data args;

		start = pfn_to_page(start_pfn);
		args.start = start;
		args.end = start + size;
		args.nid = nid;
		args.zone = zone;

		efi_memmap_walk(virtual_memmap_init, &args);
	}
}

int
ia64_pfn_valid (unsigned long pfn)
{
	char byte;
	struct page *pg = pfn_to_page(pfn);

	return     (__get_user(byte, (char __user *) pg) == 0)
		&& ((((u64)pg & PAGE_MASK) == (((u64)(pg + 1) - 1) & PAGE_MASK))
			|| (__get_user(byte, (char __user *) (pg + 1) - 1) == 0));
}
EXPORT_SYMBOL(ia64_pfn_valid);

#endif /* CONFIG_VIRTUAL_MEM_MAP */

int __init register_active_ranges(u64 start, u64 len, int nid)
{
	u64 end = start + len;

#ifdef CONFIG_KEXEC
	if (start > crashk_res.start && start < crashk_res.end)
		start = crashk_res.end;
	if (end > crashk_res.start && end < crashk_res.end)
		end = crashk_res.start;
#endif

	if (start < end)
		memblock_add_node(__pa(start), end - start, nid);
	return 0;
}

int
find_max_min_low_pfn (u64 start, u64 end, void *arg)
{
	unsigned long pfn_start, pfn_end;
#ifdef CONFIG_FLATMEM
	pfn_start = (PAGE_ALIGN(__pa(start))) >> PAGE_SHIFT;
	pfn_end = (PAGE_ALIGN(__pa(end - 1))) >> PAGE_SHIFT;
#else
	pfn_start = GRANULEROUNDDOWN(__pa(start)) >> PAGE_SHIFT;
	pfn_end = GRANULEROUNDUP(__pa(end - 1)) >> PAGE_SHIFT;
#endif
	min_low_pfn = min(min_low_pfn, pfn_start);
	max_low_pfn = max(max_low_pfn, pfn_end);
	return 0;
}

/*
 * Boot command-line option "nolwsys" can be used to disable the use of any light-weight
 * system call handler.  When this option is in effect, all fsyscalls will end up bubbling
 * down into the kernel and calling the normal (heavy-weight) syscall handler.  This is
 * useful for performance testing, but conceivably could also come in handy for debugging
 * purposes.
 */

static int nolwsys __initdata;

static int __init
nolwsys_setup (char *s)
{
	nolwsys = 1;
	return 1;
}

__setup("nolwsys", nolwsys_setup);

void __init
mem_init (void)
{
	int i;

	BUG_ON(PTRS_PER_PGD * sizeof(pgd_t) != PAGE_SIZE);
	BUG_ON(PTRS_PER_PMD * sizeof(pmd_t) != PAGE_SIZE);
	BUG_ON(PTRS_PER_PTE * sizeof(pte_t) != PAGE_SIZE);

	/*
	 * This needs to be called _after_ the command line has been parsed but
	 * _before_ any drivers that may need the PCI DMA interface are
	 * initialized or bootmem has been freed.
	 */
#ifdef CONFIG_INTEL_IOMMU
	detect_intel_iommu();
	if (!iommu_detected)
#endif
#ifdef CONFIG_SWIOTLB
		swiotlb_init(1);
#endif

#ifdef CONFIG_FLATMEM
	BUG_ON(!mem_map);
#endif

	set_max_mapnr(max_low_pfn);
	high_memory = __va(max_low_pfn * PAGE_SIZE);
	memblock_free_all();
	mem_init_print_info(NULL);

	/*
	 * For fsyscall entrpoints with no light-weight handler, use the ordinary
	 * (heavy-weight) handler, but mark it by setting bit 0, so the fsyscall entry
	 * code can tell them apart.
	 */
	for (i = 0; i < NR_syscalls; ++i) {
		extern unsigned long fsyscall_table[NR_syscalls];
		extern unsigned long sys_call_table[NR_syscalls];

		if (!fsyscall_table[i] || nolwsys)
			fsyscall_table[i] = sys_call_table[i] | 1;
	}
	setup_gate();
}

#ifdef CONFIG_MEMORY_HOTPLUG
int arch_add_memory(int nid, u64 start, u64 size,
		    struct mhp_params *params)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;
	int ret;

	if (WARN_ON_ONCE(params->pgprot.pgprot != PAGE_KERNEL.pgprot))
		return -EINVAL;

	ret = __add_pages(nid, start_pfn, nr_pages, params);
	if (ret)
		printk("%s: Problem encountered in __add_pages() as ret=%d\n",
		       __func__,  ret);

	return ret;
}

void arch_remove_memory(int nid, u64 start, u64 size,
			struct vmem_altmap *altmap)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;

	__remove_pages(start_pfn, nr_pages, altmap);
}
#endif
