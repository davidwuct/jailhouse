/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Valentine Sinitsyn, 2014, 2015
 * Copyright (c) Siemens AG, 2016
 *
 * Authors:
 *  Valentine Sinitsyn <valentine.sinitsyn@gmail.com>
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * Commands posting and event log parsing code, as well as many defines
 * were adapted from Linux's amd_iommu driver written by Joerg Roedel
 * and Leo Duran.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/cell.h>
#include <jailhouse/cell-config.h>
#include <jailhouse/control.h>
#include <jailhouse/mmio.h>
#include <jailhouse/pci.h>
#include <jailhouse/printk.h>
#include <jailhouse/string.h>
#include <asm/amd_iommu.h>
#include <asm/apic.h>
#include <asm/iommu.h>

#define CAPS_IOMMU_HEADER_REG		0x00
#define  CAPS_IOMMU_EFR_SUP		(1 << 27)
#define CAPS_IOMMU_BASE_LOW_REG		0x04
#define  CAPS_IOMMU_ENABLE		(1 << 0)
#define CAPS_IOMMU_BASE_HI_REG		0x08

#define ACPI_REPORTING_HE_SUP		(1 << 7)

#define AMD_DEV_TABLE_BASE_REG		0x0000
#define AMD_CMD_BUF_BASE_REG		0x0008
#define AMD_EVT_LOG_BASE_REG		0x0010
#define AMD_CONTROL_REG			0x0018
#define  AMD_CONTROL_IOMMU_EN		(1UL << 0)
#define  AMD_CONTROL_EVT_LOG_EN		(1UL << 2)
#define  AMD_CONTROL_EVT_INT_EN		(1UL << 3)
#define  AMD_CONTROL_COMM_WAIT_INT_EN	(1UL << 4)
#define  AMD_CONTROL_CMD_BUF_EN		(1UL << 12)
#define  AMD_CONTROL_SMIF_EN		(1UL << 22)
#define  AMD_CONTROL_SMIFLOG_EN		(1UL << 24)
#define  AMD_CONTROL_SEG_EN_MASK	BIT_MASK(36, 34)
#define  AMD_CONTROL_SEG_EN_SHIFT	34
#define AMD_EXT_FEATURES_REG		0x0030
#define  AMD_EXT_FEAT_HE_SUP		(1UL << 7)
#define  AMD_EXT_FEAT_SMI_FSUP_MASK	BIT_MASK(17, 16)
#define  AMD_EXT_FEAT_SMI_FSUP_SHIFT	16
#define  AMD_EXT_FEAT_SMI_FRC_MASK	BIT_MASK(20, 18)
#define  AMD_EXT_FEAT_SMI_FRC_SHIFT	18
#define  AMD_EXT_FEAT_SEG_SUP_MASK	BIT_MASK(39, 38)
#define  AMD_EXT_FEAT_SEG_SUP_SHIFT   	38
#define AMD_HEV_UPPER_REG		0x0040
#define AMD_HEV_LOWER_REG		0x0048
#define AMD_HEV_STATUS_REG		0x0050
#define  AMD_HEV_VALID			(1UL << 1)
#define  AMD_HEV_OVERFLOW		(1UL << 2)
#define AMD_SMI_FILTER0_REG		0x0060
#define  AMD_SMI_FILTER_VALID		(1UL << 16)
#define  AMD_SMI_FILTER_LOCKED		(1UL << 17)
#define AMD_DEV_TABLE_SEG1_REG		0x0100
#define AMD_CMD_BUF_HEAD_REG		0x2000
#define AMD_CMD_BUF_TAIL_REG		0x2008
#define AMD_EVT_LOG_HEAD_REG		0x2010
#define AMD_EVT_LOG_TAIL_REG		0x2018
#define AMD_STATUS_REG			0x2020
# define AMD_STATUS_EVT_OVERFLOW	(1UL << 0)
# define AMD_STATUS_EVT_LOG_INT		(1UL << 1)
# define AMD_STATUS_EVT_LOG_RUN		(1UL << 3)

struct dev_table_entry {
	u64 raw64[4];
} __attribute__((packed));

#define DTE_VALID			(1UL << 0)
#define DTE_TRANSLATION_VALID		(1UL << 1)
#define DTE_PAGING_MODE_4_LEVEL		(4UL << 9)
#define DTE_IR				(1UL << 61)
#define DTE_IW				(1UL << 62)

#define DEV_TABLE_SEG_MAX		8
#define DEV_TABLE_SIZE			0x200000

union buf_entry {
	u32 raw32[4];
	u64 raw64[2];
	struct {
		u32 pad0;
		u32 pad1:28;
		u32 type:4;
	};
} __attribute__((packed));

#define CMD_COMPL_WAIT			0x01
# define CMD_COMPL_WAIT_STORE		(1 << 0)
# define CMD_COMPL_WAIT_INT		(1 << 1)

#define CMD_INV_DEVTAB_ENTRY		0x02

#define CMD_INV_IOMMU_PAGES		0x03
# define CMD_INV_IOMMU_PAGES_SIZE	(1 << 0)
# define CMD_INV_IOMMU_PAGES_PDE	(1 << 1)

#define EVENT_TYPE_ILL_DEV_TAB_ENTRY	0x01
#define EVENT_TYPE_PAGE_TAB_HW_ERR	0x04
#define EVENT_TYPE_ILL_CMD_ERR		0x05
#define EVENT_TYPE_CMD_HW_ERR		0x06
#define EVENT_TYPE_IOTLB_INV_TIMEOUT	0x07
#define EVENT_TYPE_INV_PPR_REQ		0x09

#define BUF_LEN_EXPONENT_SHIFT		56

/* Allocate minimum space possible (4K or 256 entries) */
#define BUF_SIZE(name, entry)		((1UL << name##_LEN_EXPONENT) * \
					  sizeof(entry))

#define CMD_BUF_LEN_EXPONENT		8
#define EVT_LOG_LEN_EXPONENT		8

#define CMD_BUF_SIZE			BUF_SIZE(CMD_BUF, union buf_entry)
#define EVT_LOG_SIZE			BUF_SIZE(EVT_LOG, union buf_entry)

#define BITS_PER_SHORT			16

#define AMD_IOMMU_MAX_PAGE_TABLE_LEVELS	4

static struct amd_iommu {
	int idx;
	void *mmio_base;
	/* Command Buffer, Event Log */
	unsigned char *cmd_buf_base;
	unsigned char *evt_log_base;
	/* Device table */
	void *devtable_segments[DEV_TABLE_SEG_MAX];
	u8 dev_tbl_seg_sup;
	u32 cmd_tail_ptr;
	bool he_supported;
} iommu_units[JAILHOUSE_MAX_IOMMU_UNITS];

#define for_each_iommu(iommu) for (iommu = iommu_units; \
				   iommu < iommu_units + iommu_units_count; \
				   iommu++)

static unsigned int iommu_units_count;
static struct paging amd_iommu_paging[AMD_IOMMU_MAX_PAGE_TABLE_LEVELS];

/*
 * Interrupt remapping is not emulated on AMD,
 * thus we have no MMIO to intercept.
 */
unsigned int iommu_mmio_count_regions(struct cell *cell)
{
	return 0;
}

bool iommu_cell_emulates_ir(struct cell *cell)
{
	return false;
}

static int amd_iommu_init_pci(struct amd_iommu *entry,
			      struct jailhouse_iommu *iommu)
{
	u64 caps_header, hi, lo;

	/* Check alignment */
	if (iommu->size & (iommu->size - 1))
		return trace_error(-EINVAL);

	/* Check that EFR is supported */
	caps_header = pci_read_config(iommu->amd_bdf, iommu->amd_base_cap, 4);
	if (!(caps_header & CAPS_IOMMU_EFR_SUP))
		return trace_error(-EIO);

	lo = pci_read_config(iommu->amd_bdf,
			     iommu->amd_base_cap + CAPS_IOMMU_BASE_LOW_REG, 4);
	hi = pci_read_config(iommu->amd_bdf,
			     iommu->amd_base_cap + CAPS_IOMMU_BASE_HI_REG, 4);

	if (lo & CAPS_IOMMU_ENABLE &&
	    ((hi << 32) | lo) != (iommu->base | CAPS_IOMMU_ENABLE)) {
		printk("FATAL: IOMMU %d config is locked in invalid state.\n",
		       entry->idx);
		return trace_error(-EPERM);
	}

	/* Should be configured by BIOS, but we want to be sure */
	pci_write_config(iommu->amd_bdf,
			 iommu->amd_base_cap + CAPS_IOMMU_BASE_HI_REG,
			 (u32)(iommu->base >> 32), 4);
	pci_write_config(iommu->amd_bdf,
			 iommu->amd_base_cap + CAPS_IOMMU_BASE_LOW_REG,
			 (u32)(iommu->base & 0xffffffff) | CAPS_IOMMU_ENABLE,
			 4);

	/* Allocate and map MMIO space */
	entry->mmio_base = page_alloc(&remap_pool, PAGES(iommu->size));
	if (!entry->mmio_base)
		return -ENOMEM;

	return paging_create(&hv_paging_structs, iommu->base, iommu->size,
			     (unsigned long)entry->mmio_base,
			     PAGE_DEFAULT_FLAGS | PAGE_FLAG_DEVICE,
			     PAGING_NON_COHERENT);
}

static int amd_iommu_init_features(struct amd_iommu *entry,
				   struct jailhouse_iommu *iommu)
{
	u64 efr = mmio_read64(entry->mmio_base + AMD_EXT_FEATURES_REG);
	unsigned char smi_filter_regcnt;
	u64 val, ctrl_reg = 0, smi_freg = 0;
	unsigned int n;
	void *reg_base;

	/*
	 * Require SMI Filter support. Enable and lock filter but
	 * mark all entries as invalid to disable SMI delivery.
	 */
	if (!(efr & AMD_EXT_FEAT_SMI_FSUP_MASK))
		return trace_error(-EINVAL);

	/* Figure out if hardware events are supported. */
	if (iommu->amd_features)
		entry->he_supported =
			iommu->amd_features & ACPI_REPORTING_HE_SUP;
	else
		entry->he_supported = efr & AMD_EXT_FEAT_HE_SUP;

	smi_filter_regcnt = (1 << (efr & AMD_EXT_FEAT_SMI_FRC_MASK) >>
		AMD_EXT_FEAT_SMI_FRC_SHIFT);
	for (n = 0; n < smi_filter_regcnt; n++) {
		reg_base = entry->mmio_base + AMD_SMI_FILTER0_REG + (n << 3);
		smi_freg = mmio_read64(reg_base);

		if (!(smi_freg & AMD_SMI_FILTER_LOCKED)) {
			/*
			 * Program unlocked register the way we need:
			 * invalid and locked.
			 */
			mmio_write64(reg_base, AMD_SMI_FILTER_LOCKED);
		} else if (smi_freg & AMD_SMI_FILTER_VALID) {
			/*
			 * The register is locked and programed
			 * the way we don't want - error.
			 */
			printk("ERROR: SMI Filter register %d is locked "
			       "and can't be reprogrammed.\n"
			       "Reboot and check no other component uses the "
			       "IOMMU %d.\n", n, entry->idx);
			return trace_error(-EPERM);
		}
		/*
		 * The register is locked, but programmed
		 * the way we need - OK to go.
		 */
	}

	ctrl_reg |= (AMD_CONTROL_SMIF_EN | AMD_CONTROL_SMIFLOG_EN);

	/* Enable maximum Device Table segmentation possible */
	entry->dev_tbl_seg_sup = (efr & AMD_EXT_FEAT_SEG_SUP_MASK) >>
		AMD_EXT_FEAT_SEG_SUP_SHIFT;
	if (entry->dev_tbl_seg_sup) {
		val = (u64)entry->dev_tbl_seg_sup << AMD_CONTROL_SEG_EN_SHIFT;
		ctrl_reg |= val & AMD_CONTROL_SEG_EN_MASK;
	}

	mmio_write64(entry->mmio_base + AMD_CONTROL_REG, ctrl_reg);

	return 0;
}

static int amd_iommu_init_buffers(struct amd_iommu *entry,
				  struct jailhouse_iommu *iommu)
{
	/* Allocate and configure command buffer */
	entry->cmd_buf_base = page_alloc(&mem_pool, PAGES(CMD_BUF_SIZE));
	if (!entry->cmd_buf_base)
		return -ENOMEM;

	mmio_write64(entry->mmio_base + AMD_CMD_BUF_BASE_REG,
		     paging_hvirt2phys(entry->cmd_buf_base) |
		     ((u64)CMD_BUF_LEN_EXPONENT << BUF_LEN_EXPONENT_SHIFT));

	entry->cmd_tail_ptr = 0;

	/* Allocate and configure event log */
	entry->evt_log_base = page_alloc(&mem_pool, PAGES(EVT_LOG_SIZE));
	if (!entry->evt_log_base)
		return -ENOMEM;

	mmio_write64(entry->mmio_base + AMD_EVT_LOG_BASE_REG,
		     paging_hvirt2phys(entry->evt_log_base) |
		     ((u64)EVT_LOG_LEN_EXPONENT << BUF_LEN_EXPONENT_SHIFT));

	return 0;
}

static void amd_iommu_enable_command_processing(struct amd_iommu *iommu)
{
	u64 ctrl_reg;

	ctrl_reg = mmio_read64(iommu->mmio_base + AMD_CONTROL_REG);
	ctrl_reg |= AMD_CONTROL_IOMMU_EN | AMD_CONTROL_CMD_BUF_EN |
		AMD_CONTROL_EVT_LOG_EN | AMD_CONTROL_EVT_INT_EN;
	mmio_write64(iommu->mmio_base + AMD_CONTROL_REG, ctrl_reg);
}

static void amd_iommu_set_next_pt_l4(pt_entry_t pte, unsigned long next_pt)
{
	*pte = (next_pt & BIT_MASK(51, 12)) | AMD_IOMMU_PTE_PG_MODE(3) |
		AMD_IOMMU_PTE_IR | AMD_IOMMU_PTE_IW | AMD_IOMMU_PTE_P;
}

static void amd_iommu_set_next_pt_l3(pt_entry_t pte, unsigned long next_pt)
{
	*pte = (next_pt & BIT_MASK(51, 12)) | AMD_IOMMU_PTE_PG_MODE(2) |
		AMD_IOMMU_PTE_IR | AMD_IOMMU_PTE_IW | AMD_IOMMU_PTE_P;
}

static void amd_iommu_set_next_pt_l2(pt_entry_t pte, unsigned long next_pt)
{
	*pte = (next_pt & BIT_MASK(51, 12)) | AMD_IOMMU_PTE_PG_MODE(1) |
		AMD_IOMMU_PTE_IR | AMD_IOMMU_PTE_IW | AMD_IOMMU_PTE_P;
}

static unsigned long amd_iommu_get_phys_l3(pt_entry_t pte, unsigned long virt)
{
	if (*pte & AMD_IOMMU_PTE_PG_MODE_MASK)
		return INVALID_PHYS_ADDR;
	return (*pte & BIT_MASK(51, 30)) | (virt & BIT_MASK(29, 0));
}

static unsigned long amd_iommu_get_phys_l2(pt_entry_t pte, unsigned long virt)
{
	if (*pte & AMD_IOMMU_PTE_PG_MODE_MASK)
		return INVALID_PHYS_ADDR;
	return (*pte & BIT_MASK(51, 21)) | (virt & BIT_MASK(20, 0));
}

int iommu_init(void)
{
	struct jailhouse_iommu *iommu;
	struct amd_iommu *entry;
	unsigned int n;
	int err;

	iommu = &system_config->platform_info.x86.iommu_units[0];
	for (n = 0; iommu->base && n < iommu_count_units(); iommu++, n++) {
		entry = &iommu_units[iommu_units_count];

		entry->idx = n;

		/* Protect against accidental VT-d configs. */
		if (!iommu->amd_bdf)
			return trace_error(-EINVAL);

		printk("AMD IOMMU @0x%lx/0x%x\n", iommu->base, iommu->size);

		/* Initialize PCI registers and MMIO space */
		err = amd_iommu_init_pci(entry, iommu);
		if (err)
			return err;

		/* Setup IOMMU features */
		err = amd_iommu_init_features(entry, iommu);
		if (err)
			return err;

		/* Initialize command buffer and event log */
		err = amd_iommu_init_buffers(entry, iommu);
		if (err)
			return err;

		/* Enable the IOMMU */
		amd_iommu_enable_command_processing(entry);

		iommu_units_count++;
	}

	/*
	 * Derive amd_iommu_paging from very similar x86_64_paging,
	 * replicating all 4 levels.
	 */
	memcpy(amd_iommu_paging, x86_64_paging, sizeof(amd_iommu_paging));
	amd_iommu_paging[0].set_next_pt = amd_iommu_set_next_pt_l4;
	amd_iommu_paging[1].set_next_pt = amd_iommu_set_next_pt_l3;
	amd_iommu_paging[2].set_next_pt = amd_iommu_set_next_pt_l2;
	amd_iommu_paging[1].get_phys = amd_iommu_get_phys_l3;
	amd_iommu_paging[2].get_phys = amd_iommu_get_phys_l2;

	return iommu_cell_init(&root_cell);
}

int iommu_cell_init(struct cell *cell)
{
	// HACK for QEMU
	if (iommu_units_count == 0)
		return 0;

	if (cell->id > 0xffff)
		return trace_error(-ERANGE);

	cell->arch.amd_iommu.pg_structs.root_paging = amd_iommu_paging;
	cell->arch.amd_iommu.pg_structs.root_table = page_alloc(&mem_pool, 1);
	if (!cell->arch.amd_iommu.pg_structs.root_table)
		return trace_error(-ENOMEM);

	return 0;
}

static void amd_iommu_completion_wait(struct amd_iommu *iommu);

static void amd_iommu_submit_command(struct amd_iommu *iommu,
				     union buf_entry *cmd, bool draining)
{
	u32 head, next_tail, bytes_free;
	unsigned char *cur_ptr;

	head = mmio_read64(iommu->mmio_base + AMD_CMD_BUF_HEAD_REG);
	next_tail = (iommu->cmd_tail_ptr + sizeof(*cmd)) % CMD_BUF_SIZE;
	bytes_free = (head - next_tail) % CMD_BUF_SIZE;

	/* Leave space for COMPLETION_WAIT that drains the buffer. */
	if (bytes_free < (2 * sizeof(*cmd)) && !draining)
		/* Drain the buffer */
		amd_iommu_completion_wait(iommu);

	cur_ptr = &iommu->cmd_buf_base[iommu->cmd_tail_ptr];
	memcpy(cur_ptr, cmd, sizeof(*cmd));

	/* Just to be sure. */
	arch_paging_flush_cpu_caches(cur_ptr, sizeof(*cmd));

	iommu->cmd_tail_ptr =
		(iommu->cmd_tail_ptr + sizeof(*cmd)) % CMD_BUF_SIZE;
}

int iommu_map_memory_region(struct cell *cell,
			    const struct jailhouse_memory *mem)
{
	unsigned long flags = AMD_IOMMU_PTE_P;

	// HACK for QEMU
	if (iommu_units_count == 0)
		return 0;

	/*
	 * Check that the address is not outside scope of current page
	 * tables. With 4 levels, we only support 48 address bits.
	 */
	if (mem->virt_start & BIT_MASK(63, 48))
		return trace_error(-E2BIG);

	if (!(mem->flags & JAILHOUSE_MEM_DMA))
		return 0;

	if (mem->flags & JAILHOUSE_MEM_READ)
		flags |= AMD_IOMMU_PTE_IR;
	if (mem->flags & JAILHOUSE_MEM_WRITE)
		flags |= AMD_IOMMU_PTE_IW;

	return paging_create(&cell->arch.amd_iommu.pg_structs, mem->phys_start,
			mem->size, mem->virt_start, flags, PAGING_COHERENT);
}

int iommu_unmap_memory_region(struct cell *cell,
			      const struct jailhouse_memory *mem)
{
	/*
         * TODO: This is almost a complete copy of vtd.c counterpart
	 * (sans QEMU hack). Think of unification.
	 */

	// HACK for QEMU
	if (iommu_units_count == 0)
		return 0;

	if (!(mem->flags & JAILHOUSE_MEM_DMA))
		return 0;

	return paging_destroy(&cell->arch.amd_iommu.pg_structs, mem->virt_start,
			mem->size, PAGING_COHERENT);
}

static void amd_iommu_inv_dte(struct amd_iommu *iommu, u16 device_id)
{
	union buf_entry invalidate_dte = {{ 0 }};

	invalidate_dte.raw32[0] = device_id;
	invalidate_dte.type = CMD_INV_DEVTAB_ENTRY;

	amd_iommu_submit_command(iommu, &invalidate_dte, false);
}

static struct dev_table_entry *get_dev_table_entry(struct amd_iommu *iommu,
						   u16 bdf, bool allocate)
{
	struct dev_table_entry *devtable_seg;
	u8 seg_idx, seg_shift;
	u64 reg_base, reg_val;
	u16 seg_mask;
	u32 seg_size;

	if (!iommu->dev_tbl_seg_sup) {
		seg_mask = 0;
		seg_idx = 0;
		seg_size = DEV_TABLE_SIZE;
	} else {
		seg_shift = BITS_PER_SHORT - iommu->dev_tbl_seg_sup;
		seg_mask = ~((1 << seg_shift) - 1);
		seg_idx = (seg_mask & bdf) >> seg_shift;
		seg_size = DEV_TABLE_SIZE / (1 << iommu->dev_tbl_seg_sup);
	}

	/*
	 * Device table segmentation is tricky in Jailhouse. As cells can
	 * "share" the IOMMU, we don't know maximum bdf in each segment
	 * because cells are initialized independently. Thus, we can't simply
	 * adjust segment sizes for our maximum bdfs.
	 *
	 * The next best things is to lazily allocate segments as we add
	 * device using maximum possible size for segments. In the worst case
	 * scenario, we waste around 2M chunk per IOMMU.
	 */
	devtable_seg = iommu->devtable_segments[seg_idx];
	if (!devtable_seg) {
		/* If we are not permitted to allocate, just fail */
		if (!allocate)
			return NULL;

		devtable_seg = page_alloc(&mem_pool, PAGES(seg_size));
		if (!devtable_seg)
			return NULL;
		iommu->devtable_segments[seg_idx] = devtable_seg;

		if (!seg_idx)
			reg_base = AMD_DEV_TABLE_BASE_REG;
		else
			reg_base = AMD_DEV_TABLE_SEG1_REG + (seg_idx - 1) * 8;

		/* Size in Kbytes = (m + 1) * 4, see Sect 3.3.6 */
		reg_val = paging_hvirt2phys(devtable_seg) |
			(seg_size / PAGE_SIZE - 1);
		mmio_write64(iommu->mmio_base + reg_base, reg_val);
	}

	return &devtable_seg[bdf & ~seg_mask];
}

int iommu_add_pci_device(struct cell *cell, struct pci_device *device)
{
	struct dev_table_entry *dte = NULL;
	struct amd_iommu *iommu;
	u8 iommu_idx;
	int err = 0;
	u16 bdf;

	// HACK for QEMU
	if (iommu_units_count == 0)
		return 0;

	if (device->info->type == JAILHOUSE_PCI_TYPE_IVSHMEM)
		return 0;

	iommu_idx = device->info->iommu;
	if (iommu_idx > JAILHOUSE_MAX_IOMMU_UNITS) {
		err = -ERANGE;
		goto out;
	}

	iommu = &iommu_units[iommu_idx];
	bdf = device->info->bdf;

	dte = get_dev_table_entry(iommu, bdf, true);
	if (!dte) {
		err = -ENOMEM;
		goto out;
	}

	memset(dte, 0, sizeof(*dte));

	/* DomainID */
	dte->raw64[1] = cell->id & 0xffff;

	/* Translation information */
	dte->raw64[0] = DTE_IR | DTE_IW |
		paging_hvirt2phys(cell->arch.amd_iommu.pg_structs.root_table) |
		DTE_PAGING_MODE_4_LEVEL | DTE_TRANSLATION_VALID | DTE_VALID;

	/* TODO: Interrupt remapping. For now, just forward them unmapped. */

	/* Flush caches, just to be sure. */
	arch_paging_flush_cpu_caches(dte, sizeof(*dte));

	amd_iommu_inv_dte(iommu, bdf);

out:
	return trace_error(err);
}

void iommu_remove_pci_device(struct pci_device *device)
{
	struct dev_table_entry *dte = NULL;
	struct amd_iommu *iommu;
	u8 iommu_idx;
	u16 bdf;

	// HACK for QEMU
	if (iommu_units_count == 0)
		return;

	if (device->info->type == JAILHOUSE_PCI_TYPE_IVSHMEM)
		return;

	iommu_idx = device->info->iommu;
	if (iommu_idx > JAILHOUSE_MAX_IOMMU_UNITS)
		return;

	iommu = &iommu_units[iommu_idx];
	bdf = device->info->bdf;

	dte = get_dev_table_entry(iommu, bdf, false);
	if (!dte)
		return;

	/*
	 * Clear DTE_TRANSLATION_VALID, but keep the entry valid
	 * to block any DMA requests.
	 */
	dte->raw64[0] = DTE_VALID;

	/* Flush caches, just to be sure. */
	arch_paging_flush_cpu_caches(dte, sizeof(*dte));

	amd_iommu_inv_dte(iommu, bdf);
}

void iommu_cell_exit(struct cell *cell)
{
	/* TODO: Again, this a copy of vtd.c:iommu_cell_exit */
	// HACK for QEMU
	if (iommu_units_count == 0)
		return;

	page_free(&mem_pool, cell->arch.amd_iommu.pg_structs.root_table, 1);
}

static void wait_for_zero(volatile u64 *sem, unsigned long mask)
{
	while (*sem & mask)
		cpu_relax();
}

static void amd_iommu_invalidate_pages(struct amd_iommu *iommu,
				       u16 domain_id)
{
	union buf_entry invalidate_pages = {{ 0 }};

	/*
	 * Flush everything, including PDEs, in whole address range, i.e.
	 * 0x7ffffffffffff000 with S bit (see Sect. 2.2.3).
	 */
	invalidate_pages.raw32[1] = domain_id;
	invalidate_pages.raw32[2] = 0xfffff000 | CMD_INV_IOMMU_PAGES_SIZE |
		CMD_INV_IOMMU_PAGES_PDE;
	invalidate_pages.raw32[3] = 0x7fffffff;
	invalidate_pages.type = CMD_INV_IOMMU_PAGES;

	amd_iommu_submit_command(iommu, &invalidate_pages, false);
}

static void amd_iommu_completion_wait(struct amd_iommu *iommu)
{
	union buf_entry completion_wait = {{ 0 }};
	volatile u64 sem = 1;
	long addr;

	addr = paging_hvirt2phys(&sem);

	completion_wait.raw32[0] = (addr & BIT_MASK(31, 3)) |
		CMD_COMPL_WAIT_STORE;
	completion_wait.raw32[1] = (addr & BIT_MASK(51, 32)) >> 32;
	completion_wait.type = CMD_COMPL_WAIT;

	amd_iommu_submit_command(iommu, &completion_wait, true);
	mmio_write64(iommu->mmio_base + AMD_CMD_BUF_TAIL_REG,
		     iommu->cmd_tail_ptr);

	wait_for_zero(&sem, -1);
}

static void amd_iommu_init_fault_nmi(void)
{
	union pci_msi_registers msi_reg = {
		.msg32.enable = 1,
		.msg64.data = MSI_DM_NMI,
	};
	union x86_msi_vector msi_vec = {{ 0 }};
	struct per_cpu *cpu_data;
	struct amd_iommu *iommu;
	int n;

	cpu_data = iommu_select_fault_reporting_cpu();

	/* Send NMI to fault reporting CPU */
	msi_vec.native.address = MSI_ADDRESS_VALUE;
	msi_vec.native.destination = fault_reporting_cpu_id;
	msi_reg.msg64.address = msi_vec.raw.address;

	for_each_iommu(iommu) {
		struct jailhouse_iommu *cfg =
		    &system_config->platform_info.x86.iommu_units[iommu->idx];

		/* Disable MSI during interrupt reprogramming. */
		pci_write_config(cfg->amd_bdf, cfg->amd_msi_cap, 0, 4);

		/*
		 * Write new MSI capability block, re-enabling interrupts with
		 * the last word.
		 */
		for (n = 3; n >= 0; n--)
			pci_write_config(cfg->amd_bdf, cfg->amd_msi_cap + 4 * n,
					 msi_reg.raw[n], 4);
	}

	/*
	 * There is a race window in between we change fault_reporting_cpu_id
	 * and actually reprogram the MSI. To prevent event loss, signal an
	 * interrupt when done, so iommu_check_pending_faults() is called
	 * upon completion even if no further NMIs due to events would occurr.
	 *
	 * Note we can't simply use CMD_COMPL_WAIT_INT_MASK in
	 * amd_iommu_completion_wait(), as it seems that IOMMU either signal
	 * an interrupt or do memory write, but not both.
	 */
	apic_send_nmi_ipi(cpu_data);
}

void iommu_config_commit(struct cell *cell_added_removed)
{
	struct amd_iommu *iommu;

	// HACK for QEMU
	if (iommu_units_count == 0)
		return;

	/* Ensure we'll get NMI on completion, or if anything goes wrong. */
	if (cell_added_removed)
		amd_iommu_init_fault_nmi();

	for_each_iommu(iommu) {
		/* Flush caches */
		if (cell_added_removed) {
			amd_iommu_invalidate_pages(iommu,
					cell_added_removed->id & 0xffff);
			amd_iommu_invalidate_pages(iommu,
					root_cell.id & 0xffff);
		}
		/* Execute all commands in the buffer */
		amd_iommu_completion_wait(iommu);
	}
}

struct apic_irq_message iommu_get_remapped_root_int(unsigned int iommu,
						    u16 device_id,
						    unsigned int vector,
						    unsigned int remap_index)
{
	struct apic_irq_message dummy = { .valid = 0 };

	/* TODO: Implement */
	return dummy;
}

int iommu_map_interrupt(struct cell *cell, u16 device_id, unsigned int vector,
			struct apic_irq_message irq_msg)
{
	/* TODO: Implement */
	return -ENOSYS;
}

void iommu_shutdown(void)
{
	struct amd_iommu *iommu;
	u64 ctrl_reg;

	for_each_iommu(iommu) {
		/* Disable the IOMMU */
		ctrl_reg = mmio_read64(iommu->mmio_base + AMD_CONTROL_REG);
		ctrl_reg &= ~(AMD_CONTROL_IOMMU_EN | AMD_CONTROL_CMD_BUF_EN |
			AMD_CONTROL_EVT_LOG_EN | AMD_CONTROL_EVT_INT_EN);
		mmio_write64(iommu->mmio_base + AMD_CONTROL_REG, ctrl_reg);
	}
}

void iommu_check_pending_faults(void)
{
	/* TODO: Implement */
}
