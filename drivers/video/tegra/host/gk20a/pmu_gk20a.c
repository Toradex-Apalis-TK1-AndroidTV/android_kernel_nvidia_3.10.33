/*
 * drivers/video/tegra/host/gk20a/pmu_gk20a.c
 *
 * GK20A PMU (aka. gPMU outside gk20a context)
 *
 * Copyright (c) 2011-2012, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/delay.h>	/* for mdelay */
#include <linux/firmware.h>
#include <linux/nvmap.h>

#include "../dev.h"
#include "../bus_client.h"

#include "gk20a.h"
#include "hw_mc_gk20a.h"
#include "hw_pwr_gk20a.h"
#include "chip_support.h"

#define GK20A_PMU_UCODE_IMAGE	"gpmu_ucode.bin"

#define nvhost_dbg_pmu(fmt, arg...) \
	nvhost_dbg(dbg_pmu, fmt, ##arg)

static void pmu_copy_from_dmem(struct pmu_gk20a *pmu,
			u32 src, u8* dst, u32 size, u8 port)
{
	struct gk20a *g = pmu->g;
	u32 i, words, bytes;
	u32 data, addr_mask;
	u32 *dst_u32 = (u32*)dst;

	if (size == 0) {
		nvhost_err(dev_from_gk20a(g),
			"size is zero");
		return;
	}

	if (src & 0x3) {
		nvhost_err(dev_from_gk20a(g),
			"src (0x%08x) not 4-byte aligned", src);
		return;
	}

	words = size >> 2;
	bytes = size & 0x3;

	addr_mask = pwr_falcon_dmemc_offs_m() |
		    pwr_falcon_dmemc_blk_m();

	src &= addr_mask;

	gk20a_writel(g, pwr_falcon_dmemc_r(port),
		src | pwr_falcon_dmemc_aincr_f(1));

	for (i = 0; i < words; i++)
		dst_u32[i] = gk20a_readl(g, pwr_falcon_dmemd_r(port));

	if (bytes > 0) {
		data = gk20a_readl(g, pwr_falcon_dmemd_r(port));
		for (i = 0; i < bytes; i++) {
			dst[(words << 2) + i] = ((u8 *)&data)[i];
			nvhost_dbg_pmu("read: dst_u8[%d]=0x%08x",
					i, dst[(words << 2) + i]);
		}
	}

	return;
}

static void pmu_copy_to_dmem(struct pmu_gk20a *pmu,
			u32 dst, u8* src, u32 size, u8 port)
{
	struct gk20a *g = pmu->g;
	u32 i, words, bytes;
	u32 data, addr_mask;
	u32 *src_u32 = (u32*)src;

	if (size == 0) {
		nvhost_err(dev_from_gk20a(g),
			"size is zero");
		return;
	}

	if (dst & 0x3) {
		nvhost_err(dev_from_gk20a(g),
			"dst (0x%08x) not 4-byte aligned", dst);
		return;
	}

	words = size >> 2;
	bytes = size & 0x3;

	addr_mask = pwr_falcon_dmemc_offs_m() |
		    pwr_falcon_dmemc_blk_m();

	dst &= addr_mask;

	gk20a_writel(g, pwr_falcon_dmemc_r(port),
		dst | pwr_falcon_dmemc_aincw_f(1));

	for (i = 0; i < words; i++)
		gk20a_writel(g, pwr_falcon_dmemd_r(port), src_u32[i]);

	if (bytes > 0) {
		data = 0;
		for (i = 0; i < bytes; i++)
			((u8 *)&data)[i] = src[(words << 2) + i];
		gk20a_writel(g, pwr_falcon_dmemd_r(port), data);
	}

	data = gk20a_readl(g, pwr_falcon_dmemc_r(port)) & addr_mask;
	size = ALIGN(size, 4);
	if (data != dst + size) {
		nvhost_err(dev_from_gk20a(g),
			"copy failed. bytes written %d, expected %d",
			data - dst, size);
	}
	return;
}

static int pmu_idle(struct pmu_gk20a *pmu)
{
	struct gk20a *g = pmu->g;
	u32 timeout = 2000; /* 2 sec */
	u32 idle_stat;

	nvhost_dbg_fn("");

	/* wait for pmu idle */
	do {
		idle_stat = gk20a_readl(g, pwr_falcon_idlestate_r());

		if (pwr_falcon_idlestate_falcon_busy_v(idle_stat) == 0 &&
		    pwr_falcon_idlestate_ext_busy_v(idle_stat) == 0) {
			break;
		}

		if (--timeout == 0) {
			nvhost_err(dev_from_gk20a(g),
				"timeout waiting pmu idle : 0x%08x",
				idle_stat);
			return -EBUSY;
		}
		mdelay(1);
	} while (1);

	nvhost_dbg_fn("done");
	return 0;
}

static void pmu_enable_irq(struct pmu_gk20a *pmu, bool enable)
{
	struct gk20a *g = pmu->g;

	nvhost_dbg_fn("");

	gk20a_writel(g, mc_intr_mask_0_r(),
		gk20a_readl(g, mc_intr_mask_0_r()) &
		~mc_intr_mask_0_pmu_enabled_f());

	gk20a_writel(g, pwr_falcon_irqmclr_r(),
		pwr_falcon_irqmclr_gptmr_f(1)  |
		pwr_falcon_irqmclr_wdtmr_f(1)  |
		pwr_falcon_irqmclr_mthd_f(1)   |
		pwr_falcon_irqmclr_ctxsw_f(1)  |
		pwr_falcon_irqmclr_halt_f(1)   |
		pwr_falcon_irqmclr_exterr_f(1) |
		pwr_falcon_irqmclr_swgen0_f(1) |
		pwr_falcon_irqmclr_swgen1_f(1) |
		pwr_falcon_irqmclr_ext_f(0xff));

	if (enable) {
		/* dest 0=falcon, 1=host; level 0=irq0, 1=irq1 */
		gk20a_writel(g, pwr_falcon_irqdest_r(),
			pwr_falcon_irqdest_host_gptmr_f(0)    |
			pwr_falcon_irqdest_host_wdtmr_f(1)    |
			pwr_falcon_irqdest_host_mthd_f(0)     |
			pwr_falcon_irqdest_host_ctxsw_f(0)    |
			pwr_falcon_irqdest_host_halt_f(1)     |
			pwr_falcon_irqdest_host_exterr_f(0)   |
			pwr_falcon_irqdest_host_swgen0_f(1)   |
			pwr_falcon_irqdest_host_swgen1_f(0)   |
			pwr_falcon_irqdest_host_ext_f(0xff)   |
			pwr_falcon_irqdest_target_gptmr_f(1)  |
			pwr_falcon_irqdest_target_wdtmr_f(0)  |
			pwr_falcon_irqdest_target_mthd_f(0)   |
			pwr_falcon_irqdest_target_ctxsw_f(0)  |
			pwr_falcon_irqdest_target_halt_f(0)   |
			pwr_falcon_irqdest_target_exterr_f(0) |
			pwr_falcon_irqdest_target_swgen0_f(0) |
			pwr_falcon_irqdest_target_swgen1_f(1) |
			pwr_falcon_irqdest_target_ext_f(0xff));

		/* 0=disable, 1=enable */
		gk20a_writel(g, pwr_falcon_irqmset_r(),
			pwr_falcon_irqmset_gptmr_f(1)  |
			pwr_falcon_irqmset_wdtmr_f(1)  |
			pwr_falcon_irqmset_mthd_f(0)   |
			pwr_falcon_irqmset_ctxsw_f(0)  |
			pwr_falcon_irqmset_halt_f(1)   |
			pwr_falcon_irqmset_exterr_f(1) |
			pwr_falcon_irqmset_swgen0_f(1) |
			pwr_falcon_irqmset_swgen1_f(1));

		gk20a_writel(g, mc_intr_mask_0_r(),
			gk20a_readl(g, mc_intr_mask_0_r()) |
			mc_intr_mask_0_pmu_enabled_f());
	}

	nvhost_dbg_fn("done");
}

static void pmu_enable_hw(struct pmu_gk20a *pmu, bool enable)
{
	struct gk20a *g = pmu->g;
	u32 pmc_enable;

	nvhost_dbg_fn("");

	pmc_enable = gk20a_readl(g, mc_enable_r());

	if (enable)
		gk20a_writel(g, mc_enable_r(),
			pmc_enable | mc_enable_pwr_enabled_f());
	else
		gk20a_writel(g, mc_enable_r(),
			pmc_enable & ~mc_enable_pwr_enabled_f());
}

static int pmu_enable(struct pmu_gk20a *pmu, bool enable)
{
	struct gk20a *g = pmu->g;
	u32 pmc_enable;
	u32 timeout = 2000; /* 2 sec */
	int err;

	nvhost_dbg_fn("");

	if (!enable) {
		pmc_enable = gk20a_readl(g, mc_enable_r());
		if (mc_enable_pwr_v(pmc_enable) !=
		    mc_enable_pwr_disabled_v()) {

			pmu_enable_irq(pmu, false);
			pmu_enable_hw(pmu, false);

			do {
				pmc_enable = gk20a_readl(g, mc_enable_r());
				if (mc_enable_pwr_v(pmc_enable) !=
				    mc_enable_pwr_disabled_v()) {
					if (--timeout == 0) {
						nvhost_err(dev_from_gk20a(g),
							"timeout waiting pmu to reset");
						return -EBUSY;
					}
					mdelay(1);
				} else
					break;
			} while (1);
		}
	} else {
		pmu_enable_hw(pmu, true);

		/* TBD: post reset */

		err = pmu_idle(pmu);
		if (err)
			return err;

		/* just for a delay */
		gk20a_readl(g, mc_enable_r());

		pmu_enable_irq(pmu, true);
	}

	nvhost_dbg_fn("done");
	return 0;
}

static int pmu_reset(struct pmu_gk20a *pmu)
{
	int err;

	err = pmu_idle(pmu);
	if (err)
		return err;

	/* TBD: release pmu hw mutex */

	err = pmu_enable(pmu, false);
	if (err)
		return err;

	/* TBD: cancel all sequences */
	/* TBD: init all sequences and state tables */
	/* TBD: restore pre-init message handler */

	err = pmu_enable(pmu, true);
	if (err)
		return err;

	return 0;
}

static int pmu_bootstrap(struct pmu_gk20a *pmu)
{
	struct gk20a *g = pmu->g;
	struct mm_gk20a *mm = &g->mm;
	struct pmu_ucode_desc *desc = pmu->desc;
	u64 addr_code, addr_data, addr_load;
	u32 i, blocks, addr_args;
	void *ucode_ptr;

	nvhost_dbg_fn("");

	ucode_ptr = mem_op().mmap(pmu->ucode.mem.ref);
	if (IS_ERR_OR_NULL(ucode_ptr)) {
		nvhost_err(dev_from_gk20a(g),
			"fail to map pmu ucode memory");
		return -ENOMEM;
	}

	for (i = 0; i < (desc->app_start_offset + desc->app_size) >> 2; i++) {
		/* nvhost_dbg_pmu("loading pmu ucode : 0x%08x", pmu->ucode_image[i]); */
		mem_wr32(ucode_ptr, i, pmu->ucode_image[i]);
	}

	mem_op().munmap(pmu->ucode.mem.ref, ucode_ptr);

	gk20a_writel(g, pwr_falcon_itfen_r(),
		gk20a_readl(g, pwr_falcon_itfen_r()) |
		pwr_falcon_itfen_ctxen_enable_f());

	gk20a_writel(g, pwr_pmu_new_instblk_r(),
		pwr_pmu_new_instblk_ptr_f(mm->pmu.inst_block.cpu_pa >> 12) |
		pwr_pmu_new_instblk_valid_f(1) |
		pwr_pmu_new_instblk_target_fb_f());

	/* TBD: load all other surfaces */

	pmu->args.cpu_freq_HZ = 500*1000*1000; /* TBD: set correct freq */

	addr_args = (pwr_falcon_hwcfg_dmem_size_v(
		gk20a_readl(g, pwr_falcon_hwcfg_r()))
			<< GK20A_PMU_DMEM_BLKSIZE2) -
		sizeof(struct pmu_cmdline_args);

	pmu_copy_to_dmem(pmu, addr_args, (u8 *)&pmu->args,
			sizeof(struct pmu_cmdline_args), 0);

	gk20a_writel(g, pwr_falcon_dmemc_r(0),
		pwr_falcon_dmemc_offs_f(0) |
		pwr_falcon_dmemc_blk_f(0)  |
		pwr_falcon_dmemc_aincw_f(1));

	addr_code = u64_lo32((pmu->ucode.pmu_va +
			desc->app_start_offset +
			desc->app_resident_code_offset) >> 8) ;
	addr_data = u64_lo32((pmu->ucode.pmu_va +
			desc->app_start_offset +
			desc->app_resident_data_offset) >> 8);
	addr_load = u64_lo32((pmu->ucode.pmu_va +
			desc->bootloader_start_offset) >> 8);

	gk20a_writel(g, pwr_falcon_dmemd_r(0), GK20A_PMU_DMAIDX_UCODE);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), addr_code);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), desc->app_size);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), desc->app_resident_code_size);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), desc->app_imem_entry);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), addr_data);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), desc->app_resident_data_size);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), addr_code);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), 0x1);
	gk20a_writel(g, pwr_falcon_dmemd_r(0), addr_args);

	gk20a_writel(g, pwr_falcon_dmatrfbase_r(),
		addr_load - (desc->bootloader_imem_offset >> 8));

	blocks = ((desc->bootloader_size + 0xFF) & ~0xFF) >> 8;

	for (i = 0; i < blocks; i++) {
		gk20a_writel(g, pwr_falcon_dmatrfmoffs_r(),
			desc->bootloader_imem_offset + (i << 8));
		gk20a_writel(g, pwr_falcon_dmatrffboffs_r(),
			desc->bootloader_imem_offset + (i << 8));
		gk20a_writel(g, pwr_falcon_dmatrfcmd_r(),
			pwr_falcon_dmatrfcmd_imem_f(1)  |
			pwr_falcon_dmatrfcmd_write_f(0) |
			pwr_falcon_dmatrfcmd_size_f(6)  |
			pwr_falcon_dmatrfcmd_ctxdma_f(GK20A_PMU_DMAIDX_UCODE));
	}

	gk20a_writel(g, pwr_falcon_bootvec_r(),
		pwr_falcon_bootvec_vec_f(desc->bootloader_entry_point));

	gk20a_writel(g, pwr_falcon_cpuctl_r(),
		pwr_falcon_cpuctl_startcpu_f(1));

	gk20a_writel(g, pwr_falcon_os_r(), desc->app_version);

	return 0;
}

static void pmu_seq_init(struct pmu_gk20a *pmu)
{
	u32 i;

	memset(pmu->seq, 0,
		sizeof(struct pmu_sequence) * PMU_MAX_NUM_SEQUENCES);
	memset(pmu->pmu_seq_tbl, 0,
		sizeof(pmu->pmu_seq_tbl));

	for (i = 0; i < PMU_MAX_NUM_SEQUENCES; i++)
		pmu->seq[i].id = i;
}

static int pmu_seq_acquire(struct pmu_gk20a *pmu,
			struct pmu_sequence **pseq)
{
	struct gk20a *g = pmu->g;
	struct pmu_sequence *seq;
	u32 index;

	index = find_first_zero_bit(pmu->pmu_seq_tbl,
				sizeof(pmu->pmu_seq_tbl));
	if (index >= sizeof(pmu->pmu_seq_tbl)) {
		nvhost_err(dev_from_gk20a(g),
			"no free sequence available");
		return -EAGAIN;
	}
	set_bit(index, pmu->pmu_seq_tbl);

	seq = &pmu->seq[index];
	seq->state = PMU_SEQ_STATE_PENDING;

	*pseq = seq;
	return 0;
}

static void pmu_seq_release(struct pmu_gk20a *pmu,
			struct pmu_sequence *seq)
{
	seq->state	= PMU_SEQ_STATE_FREE;
	seq->desc	= PMU_INVALID_SEQ_DESC;
	seq->callback	= NULL;
	seq->cb_params	= NULL;
	seq->msg	= NULL;
	seq->out_payload = NULL;
	seq->in.size	= 0;
	seq->out.size	= 0;

	clear_bit(seq->id, pmu->pmu_seq_tbl);
}

static int pmu_queue_init(struct pmu_queue *queue,
			u32 id, struct pmu_init_msg_pmu *init)
{
	queue->id	= id;
	queue->index	= init->queue_info[id].index;
	queue->offset	= init->queue_info[id].offset;
	queue->size	= init->queue_info[id].size;

	queue->mutex_id = id;
	mutex_init(&queue->mutex);

	nvhost_dbg_pmu("queue %d: index %d, offset 0x%08x, size 0x%08x",
		id, queue->index, queue->offset, queue->size);

	return 0;
}

static int pmu_queue_head(struct pmu_gk20a *pmu, struct pmu_queue *queue,
			u32 *head, bool set)
{
	struct gk20a *g = pmu->g;

	BUG_ON(!head);

	if (PMU_IS_COMMAND_QUEUE(queue->id)) {

		if (queue->index >= pwr_pmu_queue_head__size_1_v())
			return -EINVAL;

		if (!set)
			*head = pwr_pmu_queue_head_address_v(
				gk20a_readl(g,
					pwr_pmu_queue_head_r(queue->index)));
		else
			gk20a_writel(g,
				pwr_pmu_queue_head_r(queue->index),
				pwr_pmu_queue_head_address_f(*head));
	} else {
		if (!set)
			*head = pwr_pmu_msgq_head_val_v(
				gk20a_readl(g, pwr_pmu_msgq_head_r()));
		else
			gk20a_writel(g,
				pwr_pmu_msgq_head_r(),
				pwr_pmu_msgq_head_val_f(*head));
	}

	return 0;
}

static int pmu_queue_tail(struct pmu_gk20a *pmu, struct pmu_queue *queue,
			u32 *tail, bool set)
{
	struct gk20a *g = pmu->g;

	BUG_ON(!tail);

	if (PMU_IS_COMMAND_QUEUE(queue->id)) {

		if (queue->index >= pwr_pmu_queue_tail__size_1_v())
			return -EINVAL;

		if (!set)
			*tail = pwr_pmu_queue_tail_address_v(
				gk20a_readl(g,
					pwr_pmu_queue_tail_r(queue->index)));
		else
			gk20a_writel(g,
				pwr_pmu_queue_tail_r(queue->index),
				pwr_pmu_queue_tail_address_f(*tail));
	} else {
		if (!set)
			*tail = pwr_pmu_msgq_tail_val_v(
				gk20a_readl(g, pwr_pmu_msgq_tail_r()));
		else
			gk20a_writel(g,
				pwr_pmu_msgq_tail_r(),
				pwr_pmu_msgq_tail_val_f(*tail));
	}

	return 0;
}

static inline void pmu_queue_read(struct pmu_gk20a *pmu,
			u32 offset, u8 *dst, u32 size)
{
	pmu_copy_from_dmem(pmu, offset, dst, size, 0);
}

static inline void pmu_queue_write(struct pmu_gk20a *pmu,
			u32 offset, u8 *src, u32 size)
{
	pmu_copy_to_dmem(pmu, offset, src, size, 0);
}

static int pmu_mutex_acquire(struct pmu_gk20a *pmu, u32 id, u32 *token)
{
	struct gk20a *g = pmu->g;
	struct pmu_mutex *mutex;
	u32 owner, data;
	bool acquired = false;
	int err;

	BUG_ON(!token);
	BUG_ON(!PMU_MUTEX_ID_IS_VALID(id));
	BUG_ON(id > pmu->mutex_cnt);

	mutex = &pmu->mutex[id];

	owner = pwr_pmu_mutex_value_v(
		gk20a_readl(g, pwr_pmu_mutex_r(mutex->index)));

	if (*token != PMU_INVALID_MUTEX_OWNER_ID && *token == owner) {
		nvhost_dbg_pmu("already acquired by owner : 0x%08x", *token);
		mutex->ref_cnt++;
		return 0;
	}

	do {
		data = gk20a_readl(g, pwr_pmu_mutex_id_r());
		owner = pwr_pmu_mutex_id_value_v(data);
		if (owner == pwr_pmu_mutex_id_value_init_v() ||
		    owner == pwr_pmu_mutex_id_value_not_avail_v()) {
			nvhost_warn(dev_from_gk20a(g),
				"fail to generate mutex token: val 0x%08x",
				data);
			continue;
		}

		gk20a_writel(g, pwr_pmu_mutex_r(mutex->index),
			pwr_pmu_mutex_value_f(owner));

		data = pwr_pmu_mutex_value_v(
			gk20a_readl(g, pwr_pmu_mutex_r(mutex->index)));

		if (data == owner) {
			acquired = true;
			mutex->ref_cnt = 1;
			mutex->acquired = 1;
		} else {
			nvhost_warn(dev_from_gk20a(g),
				"fail to acquire mutex idx=0x%08x",
				mutex->index);

			gk20a_writel(g,
				pwr_pmu_mutex_id_r(),
				pwr_pmu_mutex_id_release_value_f(mutex->index));
			continue;
		}
	} while (!acquired);

	return err;
}

static int pmu_mutex_release(struct pmu_gk20a *pmu, u32 id, u32 *token)
{
	struct gk20a *g = pmu->g;
	struct pmu_mutex *mutex;
	u32 owner;

	BUG_ON(!token);
	BUG_ON(!PMU_MUTEX_ID_IS_VALID(id));
	BUG_ON(id > pmu->mutex_cnt);

	mutex = &pmu->mutex[id];

	owner = pwr_pmu_mutex_value_v(
		gk20a_readl(g, pwr_pmu_mutex_r(mutex->index)));

	if (*token != owner) {
		nvhost_err(dev_from_gk20a(g),
			"requester 0x%08x NOT match owner 0x%08x",
			*token, owner);
		return -EINVAL;
	}

	if (!mutex->acquired || --mutex->ref_cnt == 0) {
		gk20a_writel(g, pwr_pmu_mutex_r(mutex->index),
			pwr_pmu_mutex_value_initial_lock_f());
		gk20a_writel(g, pwr_pmu_mutex_id_r(),
			pwr_pmu_mutex_id_release_value_f(owner));
	}

	return 0;
}

static int pmu_queue_lock(struct pmu_gk20a *pmu,
			struct pmu_queue *queue)
{
	int err;

	if (PMU_IS_MESSAGE_QUEUE(queue->id))
		return 0;

	if (PMU_IS_SW_COMMAND_QUEUE(queue->id)) {
		mutex_lock(&queue->mutex);
		queue->locked = true;
		return 0;
	}

	err = pmu_mutex_acquire(pmu, queue->mutex_id,
			&queue->mutex_lock);
	if (err == 0)
		queue->locked = true;

	return err;
}

static int pmu_queue_unlock(struct pmu_gk20a *pmu,
			struct pmu_queue *queue)
{
	int err;

	if (PMU_IS_MESSAGE_QUEUE(queue->id))
		return 0;

	if (PMU_IS_SW_COMMAND_QUEUE(queue->id)) {
		mutex_unlock(&queue->mutex);
		queue->locked = false;
		return 0;
	}

	if (queue->locked) {
		err = pmu_mutex_release(pmu, queue->mutex_id,
				&queue->mutex_lock);
		if (err == 0)
			queue->locked = false;
	}

	return 0;
}

/* called by pmu_read_message, no lock */
static bool pmu_queue_is_empty(struct pmu_gk20a *pmu,
			struct pmu_queue *queue)
{
	u32 head, tail;

	pmu_queue_head(pmu, queue, &head, QUEUE_GET);
	if (queue->opened && queue->oflag == OFLAG_READ)
		tail = queue->position;
	else
		pmu_queue_tail(pmu, queue, &tail, QUEUE_GET);

	return head == tail;
}

static bool pmu_queue_has_room(struct pmu_gk20a *pmu,
			struct pmu_queue *queue, u32 size, bool *need_rewind)
{
	u32 head, tail, free;
	bool rewind = false;

	BUG_ON(!queue->locked);

	size = ALIGN(size, QUEUE_ALIGNMENT);

	pmu_queue_head(pmu, queue, &head, QUEUE_GET);
	pmu_queue_tail(pmu, queue, &tail, QUEUE_GET);

	if (head >= tail) {
		free = queue->offset + queue->size - head;
		free -= PMU_CMD_HDR_SIZE;

		if (size > free) {
			rewind = true;
			head = queue->offset;
		}
	}

	if (head < tail)
		free = tail - head - 1;

	if (need_rewind)
		*need_rewind = rewind;

	return size <= free;
}

static int pmu_queue_push(struct pmu_gk20a *pmu,
			struct pmu_queue *queue, void *data, u32 size)
{
	nvhost_dbg_fn("");

	if (!queue->opened && queue->oflag == OFLAG_WRITE){
		nvhost_err(dev_from_gk20a(pmu->g),
			"queue not opened for write");
		return -EINVAL;
	}

	pmu_queue_write(pmu, queue->position, data, size);
	queue->position += ALIGN(size, QUEUE_ALIGNMENT);
	return 0;
}

static int pmu_queue_pop(struct pmu_gk20a *pmu,
			struct pmu_queue *queue, void *data, u32 size,
			u32 *bytes_read)
{
	u32 head, tail, used;

	*bytes_read = 0;

	if (!queue->opened && queue->oflag == OFLAG_READ){
		nvhost_err(dev_from_gk20a(pmu->g),
			"queue not opened for read");
		return -EINVAL;
	}

	pmu_queue_head(pmu, queue, &head, QUEUE_GET);
	tail = queue->position;

	if (head == tail)
		return 0;

	if (head > tail)
		used = head - tail;
	else
		used = queue->offset + queue->size - tail;

	if (size > used) {
		nvhost_warn(dev_from_gk20a(pmu->g),
			"queue size smaller than request read");
		size = used;
	}

	pmu_queue_read(pmu, tail, data, size);
	queue->position += ALIGN(size, QUEUE_ALIGNMENT);
	*bytes_read = size;
	return 0;
}

static void pmu_queue_rewind(struct pmu_gk20a *pmu,
			struct pmu_queue *queue)
{
	struct pmu_cmd cmd;

	nvhost_dbg_fn("");

	if (!queue->opened) {
		nvhost_err(dev_from_gk20a(pmu->g),
			"queue not opened");
		return;
	}

	if (queue->oflag == OFLAG_WRITE) {
		cmd.hdr.unit_id = PMU_UNIT_REWIND;
		cmd.hdr.size = PMU_CMD_HDR_SIZE;
		pmu_queue_push(pmu, queue, &cmd, cmd.hdr.size);
		nvhost_dbg_pmu("queue %d rewinded", queue->id);
	}

	queue->position = queue->offset;
	return;
}

/* open for read and lock the queue */
static int pmu_queue_open_read(struct pmu_gk20a *pmu,
			struct pmu_queue *queue)
{
	int err;

	err = pmu_queue_lock(pmu, queue);
	if (err)
		return err;

	if (queue->opened)
		BUG();

	pmu_queue_tail(pmu, queue, &queue->position, QUEUE_GET);
	queue->oflag = OFLAG_READ;
	queue->opened = true;

	return 0;
}

/* open for write and lock the queue
   make sure there's enough free space for the write */
static int pmu_queue_open_write(struct pmu_gk20a *pmu,
			struct pmu_queue *queue, u32 size)
{
	bool rewind = false;
	int err;

	err = pmu_queue_lock(pmu, queue);
	if (err)
		return err;

	if (queue->opened)
		BUG();

	if (!pmu_queue_has_room(pmu, queue, size, &rewind)) {
		nvhost_err(dev_from_gk20a(pmu->g), "queue full");
		return -EAGAIN;
	}

	pmu_queue_head(pmu, queue, &queue->position, QUEUE_GET);
	queue->oflag = OFLAG_WRITE;
	queue->opened = true;

	if (rewind)
		pmu_queue_rewind(pmu, queue);

	return 0;
}

/* close and unlock the queue */
static int pmu_queue_close(struct pmu_gk20a *pmu,
			struct pmu_queue *queue, bool commit)
{
	if (!queue->opened)
		return 0;

	if (commit) {
		if (queue->oflag == OFLAG_READ) {
			pmu_queue_tail(pmu, queue,
				&queue->position, QUEUE_SET);
		}
		else {
			pmu_queue_head(pmu, queue,
				&queue->position, QUEUE_SET);
		}
	}

	queue->opened = false;

	pmu_queue_unlock(pmu, queue);

	return 0;
}

void gk20a_remove_pmu_support(struct gk20a *g, struct pmu_gk20a *pmu)
{
	nvhost_dbg_fn("");
	/* TBD */
}

int gk20a_init_pmu_reset_enable_hw(struct gk20a *g)
{
	struct pmu_gk20a *pmu = &g->pmu;

	nvhost_dbg_fn("");

	pmu_enable_hw(pmu, true);

	return 0;
}

static void pmu_elpg_enable_allow(unsigned long arg)
{
	struct pmu_gk20a *pmu = (struct pmu_gk20a *)arg;
	struct gk20a *g = pmu->g;

	nvhost_dbg_fn("");

	pmu->elpg_enable_allow = true;
	if (pmu->elpg_stat == PMU_ELPG_STAT_OFF_ON_PENDING)
		gk20a_pmu_enable_elpg(g);
}

int gk20a_init_pmu_setup_sw(struct gk20a *g, bool reinit)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct mm_gk20a *mm = &g->mm;
	struct vm_gk20a *vm = &mm->pmu.vm;
	struct device *d = dev_from_gk20a(g);
	struct mem_mgr *memmgr = mem_mgr_from_mm(mm);
	const struct firmware *ucode_fw = NULL;
	u32 size;
	int i, err = 0;
	u8 *ptr;

	nvhost_dbg_fn("");

	/* no infoRom script from vbios? */

	/* TBD: sysmon subtask */

	pmu->mutex_cnt = pwr_pmu_mutex__size_1_v();
	pmu->mutex = kzalloc(pmu->mutex_cnt *
		sizeof(struct pmu_mutex), GFP_KERNEL);
	if (!pmu->mutex) {
		err = -ENOMEM;
		goto clean_up;
	}

	for (i = 0; i < pmu->mutex_cnt; i++) {
		pmu->mutex[i].id    = i;
		pmu->mutex[i].index = i;
	}

	pmu->seq = kzalloc(PMU_MAX_NUM_SEQUENCES *
		sizeof(struct pmu_sequence), GFP_KERNEL);
	if (!pmu->seq) {
		err = -ENOMEM;
		goto clean_up;
	}

	pmu_seq_init(pmu);

	ucode_fw = nvhost_client_request_firmware(g->dev, GK20A_PMU_UCODE_IMAGE);
	if (IS_ERR_OR_NULL(ucode_fw)) {
		nvhost_err(d, "failed to load pmu ucode!!");
		err = -ENOENT;
		return err;
	}

	nvhost_dbg_fn("firmware loaded");

	pmu->desc = (struct pmu_ucode_desc *)ucode_fw->data;
	pmu->ucode_image = (u32 *)((u8 *)pmu->desc +
			pmu->desc->descriptor_size);

	gk20a_init_pmu_vm(mm);

	pmu->ucode.mem.ref = mem_op().alloc(memmgr,
			GK20A_PMU_UCODE_SIZE_MAX,
			DEFAULT_NVMAP_ALLOC_ALIGNMENT,
			DEFAULT_NVMAP_ALLOC_FLAGS,
			NVMAP_HEAP_CARVEOUT_GENERIC);
	if (IS_ERR_OR_NULL(pmu->ucode.mem.ref)) {
		err = -ENOMEM;
		goto clean_up;
	}

	pmu->ucode.pmu_va = vm->map(vm, memmgr, pmu->ucode.mem.ref,
			/*offset_align, flags, kind*/
			0, 0, 0);
	if (!pmu->ucode.pmu_va) {
		nvhost_err(d, "failed to map pmu ucode memory!!");
		return err;
	}

	init_waitqueue_head(&pmu->pg_wq);

	size = 0;
	err = gr_gk20a_fecs_get_reglist_img_size(g, &size);
	if (err) {
		nvhost_err(dev_from_gk20a(g),
			"fail to query fecs pg buffer size");
		goto clean_up;
	}

	pmu->pg_buf.mem.ref = mem_op().alloc(memmgr, size,
				DEFAULT_NVMAP_ALLOC_ALIGNMENT, /* TBD: 256 bytes alignment is sufficient */
				DEFAULT_NVMAP_ALLOC_FLAGS,
				NVMAP_HEAP_CARVEOUT_GENERIC);
	if (IS_ERR_OR_NULL(pmu->pg_buf.mem.ref)) {
		nvhost_err(dev_from_gk20a(g),
			"fail to allocate fecs pg buffer");
		err = -ENOMEM;
		goto clean_up;
	}
	pmu->pg_buf.mem.size = size;

	pmu->pg_buf.pmu_va = vm->map(vm, memmgr, pmu->pg_buf.mem.ref,
			 /*offset_align, flags, kind*/
			0, 0, 0);
	if (!pmu->pg_buf.pmu_va) {
		nvhost_err(d, "failed to map fecs pg buffer");
		err = -ENOMEM;
		goto clean_up;
	}

	pmu->seq_buf.mem.ref = mem_op().alloc(memmgr, 4096,
				DEFAULT_NVMAP_ALLOC_ALIGNMENT,
				DEFAULT_NVMAP_ALLOC_FLAGS,
				NVMAP_HEAP_CARVEOUT_GENERIC);
	if (IS_ERR_OR_NULL(pmu->seq_buf.mem.ref)) {
		nvhost_err(dev_from_gk20a(g),
			"fail to allocate zbc buffer");
		err = -ENOMEM;
		goto clean_up;
	}

	pmu->seq_buf.pmu_va = vm->map(vm, memmgr, pmu->seq_buf.mem.ref,
			/*offset_align, flags, kind*/
			0, 0, 0);
	if (!pmu->seq_buf.pmu_va) {
		nvhost_err(d, "failed to map zbc buffer");
		err = -ENOMEM;
		goto clean_up;
	}

	ptr = (u8 *)mem_op().mmap(pmu->seq_buf.mem.ref);
	if (IS_ERR_OR_NULL(ptr)) {
		nvhost_err(d, "failed to map cpu ptr for zbc buffer");
		goto clean_up;
	}

	/* TBD: remove this if ZBC save/restore is handled by PMU
	   send an empty ZBC sequence for now */
	ptr[0] = 0x16; /* opcode EXIT */
	ptr[1] = 0; ptr[2] = 1; ptr[3] = 0;
	ptr[4] = 0; ptr[5] = 0; ptr[6] = 0; ptr[7] = 0;

	pmu->seq_buf.mem.size = 8;

	mem_op().munmap(pmu->seq_buf.mem.ref, ptr);

	init_timer(&pmu->elpg_timer);
	pmu->elpg_timer.function = pmu_elpg_enable_allow;
	pmu->elpg_timer.data = (unsigned long)pmu;

	pmu->remove_support = gk20a_remove_pmu_support;

	nvhost_dbg_fn("done");
	return 0;

clean_up:
	nvhost_dbg_fn("fail");
	if (ucode_fw)
		release_firmware(ucode_fw);
	kfree(pmu->mutex);
	kfree(pmu->seq);
	vm->unmap(vm, pmu->ucode.pmu_va);
	vm->unmap(vm, pmu->pg_buf.pmu_va);
	vm->unmap(vm, pmu->seq_buf.pmu_va);
	mem_op().put(memmgr, pmu->ucode.mem.ref);
	mem_op().put(memmgr, pmu->pg_buf.mem.ref);
	mem_op().put(memmgr, pmu->seq_buf.mem.ref);
	return err;
}

static void pmu_handle_pg_elpg_msg(struct gk20a *g, struct pmu_msg *msg,
			void *param, u32 handle, u32 status);

static void pmu_handle_pg_buf_config_msg(struct gk20a *g, struct pmu_msg *msg,
			void *param, u32 handle, u32 status)
{
	struct pmu_gk20a *pmu = param;
	struct pmu_pg_msg_eng_buf_stat *eng_buf_stat = &msg->msg.pg.eng_buf_stat;

	nvhost_dbg_fn("");

	if (status != 0) {
		nvhost_err(dev_from_gk20a(g), "PGENG cmd aborted");
		/* TBD: disable ELPG */
		return;
	}

	if (eng_buf_stat->status == PMU_PG_MSG_ENG_BUF_FAILED) {
		nvhost_err(dev_from_gk20a(g), "failed to load PGENG buffer");
	}

	pmu->pg_buf_loaded = (eng_buf_stat->status == PMU_PG_MSG_ENG_BUF_LOADED);
	wake_up(&pmu->pg_wq);
}

int gk20a_init_pmu_setup_hw(struct gk20a *g)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct mm_gk20a *mm = &g->mm;
	struct pmu_cmd cmd;
	u32 desc;
	int remain, err;
	bool status;

	nvhost_dbg_fn("");

	pmu_reset(pmu);

	/* setup apertures - virtual */
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_UCODE),
		pwr_fbif_transcfg_mem_type_virtual_f());
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_VIRT),
		pwr_fbif_transcfg_mem_type_virtual_f());
	/* setup apertures - physical */
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_PHYS_VID),
		pwr_fbif_transcfg_mem_type_physical_f() |
		pwr_fbif_transcfg_target_local_fb_f());
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_PHYS_SYS_COH),
		pwr_fbif_transcfg_mem_type_physical_f() |
		pwr_fbif_transcfg_target_coherent_sysmem_f());
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_PHYS_SYS_NCOH),
		pwr_fbif_transcfg_mem_type_physical_f() |
		pwr_fbif_transcfg_target_noncoherent_sysmem_f());

	/* TBD: acquire pmu hw mutex */

	/* TBD: load pmu ucode */
	err = pmu_bootstrap(pmu);
	if (err)
		return err;

	/* TBD: post reset again? */

	/* PMU_INIT message handler will send PG_INIT */
	remain = wait_event_interruptible_timeout(
			pmu->pg_wq,
			(status = (pmu->elpg_ready &&
				pmu->stat_dmem_offset != 0 &&
				pmu->elpg_stat == PMU_ELPG_STAT_OFF)),
			2 * HZ /* 2 sec */);
	if (status == 0) {
		nvhost_err(dev_from_gk20a(g),
			"PG_INIT_ACK failed, remaining timeout : 0x%08x", remain);
		return -EBUSY;
	}

	pmu->elpg_enable_allow = true;

	err = gr_gk20a_fecs_set_reglist_bind_inst(g, mm->pmu.inst_block.cpu_pa);
	if (err) {
		nvhost_err(dev_from_gk20a(g),
			"fail to bind pmu inst to gr");
		return err;
	}

	err = gr_gk20a_fecs_set_reglist_virual_addr(g, pmu->pg_buf.pmu_va);
	if (err) {
		nvhost_err(dev_from_gk20a(g),
			"fail to set pg buffer pmu va");
		return err;
	}

	memset(&cmd, 0, sizeof(struct pmu_cmd));
	cmd.hdr.unit_id = PMU_UNIT_PG;
	cmd.hdr.size = PMU_CMD_HDR_SIZE + sizeof(struct pmu_pg_cmd_eng_buf_load);
	cmd.cmd.pg.eng_buf_load.cmd_type = PMU_PG_CMD_TYPE_ENG_BUF_LOAD;
	cmd.cmd.pg.eng_buf_load.engine_id = ENGINE_GR_GK20A;
	cmd.cmd.pg.eng_buf_load.buf_idx = PMU_PGENG_GR_BUFFER_IDX_FECS;
	cmd.cmd.pg.eng_buf_load.buf_size = pmu->pg_buf.mem.size;
	cmd.cmd.pg.eng_buf_load.dma_base = u64_lo32(pmu->pg_buf.pmu_va >> 8);
	cmd.cmd.pg.eng_buf_load.dma_offset = (u8)(pmu->pg_buf.pmu_va & 0xFF);
	cmd.cmd.pg.eng_buf_load.dma_idx = PMU_DMAIDX_VIRT;

	gk20a_pmu_cmd_post(g, &cmd, NULL, NULL, PMU_COMMAND_QUEUE_LPQ,
			pmu_handle_pg_buf_config_msg, pmu, &desc, ~0);

	remain = wait_event_interruptible_timeout(
			pmu->pg_wq,
			pmu->pg_buf_loaded,
			2 * HZ /* 2 sec */);
	if (!pmu->pg_buf_loaded) {
		nvhost_err(dev_from_gk20a(g),
			"PGENG FECS buffer load failed, remaining timeout : 0x%08x",
			remain);
		return -EBUSY;
	}

	memset(&cmd, 0, sizeof(struct pmu_cmd));
	cmd.hdr.unit_id = PMU_UNIT_PG;
	cmd.hdr.size = PMU_CMD_HDR_SIZE + sizeof(struct pmu_pg_cmd_eng_buf_load);
	cmd.cmd.pg.eng_buf_load.cmd_type = PMU_PG_CMD_TYPE_ENG_BUF_LOAD;
	cmd.cmd.pg.eng_buf_load.engine_id = ENGINE_GR_GK20A;
	cmd.cmd.pg.eng_buf_load.buf_idx = PMU_PGENG_GR_BUFFER_IDX_ZBC;
	cmd.cmd.pg.eng_buf_load.buf_size = pmu->seq_buf.mem.size;
	cmd.cmd.pg.eng_buf_load.dma_base = u64_lo32(pmu->seq_buf.pmu_va >> 8);
	cmd.cmd.pg.eng_buf_load.dma_offset = (u8)(pmu->seq_buf.pmu_va & 0xFF);
	cmd.cmd.pg.eng_buf_load.dma_idx = PMU_DMAIDX_VIRT;

	gk20a_pmu_cmd_post(g, &cmd, NULL, NULL, PMU_COMMAND_QUEUE_LPQ,
			pmu_handle_pg_buf_config_msg, pmu, &desc, ~0);

	remain = wait_event_interruptible_timeout(
			pmu->pg_wq,
			pmu->pg_buf_loaded,
			2 * HZ /* 2 sec */);
	if (!pmu->pg_buf_loaded) {
		nvhost_err(dev_from_gk20a(g),
			"PGENG ZBC buffer load failed, remaining timeout 0x%08x",
			remain);
		return -EBUSY;
	}

	return 0;
}

int gk20a_init_pmu_support(struct gk20a *g, bool reinit)
{
	struct pmu_gk20a *pmu = &g->pmu;
	u32 err;

	nvhost_dbg_fn("");

	if (pmu->initialized)
		return 0;

	pmu->g = g;

	err = gk20a_init_pmu_reset_enable_hw(g);
	if (err)
		return err;

	if (support_gk20a_pmu()) {
		err = gk20a_init_pmu_setup_sw(g, reinit);
		if (err)
			return err;

		err = gk20a_init_pmu_setup_hw(g);
		if (err)
			return err;

		pmu->initialized = true;
	}

	return err;
}

static void pmu_handle_pg_elpg_msg(struct gk20a *g, struct pmu_msg *msg,
			void *param, u32 handle, u32 status)
{
	struct pmu_gk20a *pmu = param;
	struct pmu_pg_msg_elpg_msg *elpg_msg = &msg->msg.pg.elpg_msg;

	nvhost_dbg_fn("");

	if (status != 0) {
		nvhost_err(dev_from_gk20a(g), "ELPG cmd aborted");
		/* TBD: disable ELPG */
		return;
	}

	switch (elpg_msg->msg) {
	case PMU_PG_ELPG_MSG_INIT_ACK:
		nvhost_dbg_pmu("INIT_PG is acknowledged from PMU");
		pmu->elpg_ready = true;
		wake_up(&pmu->pg_wq);
		break;
	case PMU_PG_ELPG_MSG_ALLOW_ACK:
		nvhost_dbg_pmu("ALLOW is acknowledged from PMU");
		pmu->elpg_stat = PMU_ELPG_STAT_ON;
		wake_up(&pmu->pg_wq);
		break;
	case PMU_PG_ELPG_MSG_DISALLOW_ACK:
		nvhost_dbg_pmu("DISALLOW is acknowledged from PMU");
		pmu->elpg_stat = PMU_ELPG_STAT_OFF;
		wake_up(&pmu->pg_wq);
		break;
	default:
		nvhost_err(dev_from_gk20a(g),
			"unsupported ELPG message : 0x%04x", elpg_msg->msg);
	}

	return;
}

static void pmu_handle_pg_stat_msg(struct gk20a *g, struct pmu_msg *msg,
			void *param, u32 handle, u32 status)
{
	struct pmu_gk20a *pmu = param;

	nvhost_dbg_fn("");

	if (status != 0) {
		nvhost_err(dev_from_gk20a(g), "ELPG cmd aborted");
		/* TBD: disable ELPG */
		return;
	}

	switch (msg->msg.pg.stat.sub_msg_id) {
	case PMU_PG_STAT_MSG_RESP_DMEM_OFFSET:
		nvhost_dbg_pmu("ALLOC_DMEM_OFFSET is acknowledged from PMU");
		pmu->stat_dmem_offset = msg->msg.pg.stat.data;
		wake_up(&pmu->pg_wq);
		break;
	default:
		break;
	}
}

static int pmu_init_powergating(struct pmu_gk20a *pmu)
{
	struct gk20a *g = pmu->g;
	struct pmu_cmd cmd;
	u32 seq;

	nvhost_dbg_fn("");

	/* TBD: calculate threshold for silicon */
	gk20a_writel(g, pwr_pmu_pg_idlefilth_r(ENGINE_GR_GK20A),
		PMU_PG_IDLE_THRESHOLD);
	gk20a_writel(g, pwr_pmu_pg_ppuidlefilth_r(ENGINE_GR_GK20A),
		PMU_PG_POST_POWERUP_IDLE_THRESHOLD);

	/* init ELPG */
	memset(&cmd, 0, sizeof(struct pmu_cmd));
	cmd.hdr.unit_id = PMU_UNIT_PG;
	cmd.hdr.size = PMU_CMD_HDR_SIZE + sizeof(struct pmu_pg_cmd_elpg_cmd);
	cmd.cmd.pg.elpg_cmd.cmd_type = PMU_PG_CMD_TYPE_ELPG_CMD;
	cmd.cmd.pg.elpg_cmd.engine_id = ENGINE_GR_GK20A;
	cmd.cmd.pg.elpg_cmd.cmd = PMU_PG_ELPG_CMD_INIT;

	gk20a_pmu_cmd_post(g, &cmd, NULL, NULL, PMU_COMMAND_QUEUE_HPQ,
			pmu_handle_pg_elpg_msg, pmu, &seq, ~0);

	/* alloc dmem for powergating state log */
	pmu->stat_dmem_offset = 0;
	memset(&cmd, 0, sizeof(struct pmu_cmd));
	cmd.hdr.unit_id = PMU_UNIT_PG;
	cmd.hdr.size = PMU_CMD_HDR_SIZE + sizeof(struct pmu_pg_cmd_stat);
	cmd.cmd.pg.stat.cmd_type = PMU_PG_CMD_TYPE_PG_STAT;
	cmd.cmd.pg.stat.engine_id = ENGINE_GR_GK20A;
	cmd.cmd.pg.stat.sub_cmd_id = PMU_PG_STAT_CMD_ALLOC_DMEM;
	cmd.cmd.pg.stat.data = 0;

	gk20a_pmu_cmd_post(g, &cmd, NULL, NULL, PMU_COMMAND_QUEUE_LPQ,
			pmu_handle_pg_stat_msg, pmu, &seq, ~0);

	/* disallow ELPG initially
	   PMU ucode requires a disallow cmd before allow cmd */
	pmu->elpg_stat = PMU_ELPG_STAT_ON; /* set for wait_event PMU_ELPG_STAT_OFF */
	memset(&cmd, 0, sizeof(struct pmu_cmd));
	cmd.hdr.unit_id = PMU_UNIT_PG;
	cmd.hdr.size = PMU_CMD_HDR_SIZE + sizeof(struct pmu_pg_cmd_elpg_cmd);
	cmd.cmd.pg.elpg_cmd.cmd_type = PMU_PG_CMD_TYPE_ELPG_CMD;
	cmd.cmd.pg.elpg_cmd.engine_id = ENGINE_GR_GK20A;
	cmd.cmd.pg.elpg_cmd.cmd = PMU_PG_ELPG_CMD_DISALLOW;

	gk20a_pmu_cmd_post(g, &cmd, NULL, NULL, PMU_COMMAND_QUEUE_HPQ,
			pmu_handle_pg_elpg_msg, pmu, &seq, ~0);

	return 0;
}

static int pmu_process_init_msg(struct pmu_gk20a *pmu,
			struct pmu_msg *msg)
{
	struct gk20a *g = pmu->g;
	struct pmu_init_msg_pmu *init;
	struct pmu_sha1_gid_data gid_data;
	u32 i, tail = 0;

	tail = pwr_pmu_msgq_tail_val_v(
		gk20a_readl(g, pwr_pmu_msgq_tail_r()));

	pmu_copy_from_dmem(pmu, tail,
		(u8 *)&msg->hdr, PMU_MSG_HDR_SIZE, 0);

	if (msg->hdr.unit_id != PMU_UNIT_INIT) {
		nvhost_err(dev_from_gk20a(g),
			"expecting init msg");
		return -EINVAL;
	}

	pmu_copy_from_dmem(pmu, tail + PMU_MSG_HDR_SIZE,
		(u8 *)&msg->msg, msg->hdr.size - PMU_MSG_HDR_SIZE, 0);

	if (msg->msg.init.msg_type != PMU_INIT_MSG_TYPE_PMU_INIT) {
		nvhost_err(dev_from_gk20a(g),
			"expecting init msg");
		return -EINVAL;
	}

	tail += ALIGN(msg->hdr.size, PMU_DMEM_ALIGNMENT);
	gk20a_writel(g, pwr_pmu_msgq_tail_r(),
		pwr_pmu_msgq_tail_val_f(tail));

	if (!pmu->gid_info.valid) {

		pmu_copy_from_dmem(pmu,
			msg->msg.init.pmu_init.sw_managed_area_offset,
			(u8 *)&gid_data,
			sizeof(struct pmu_sha1_gid_data), 0);

		pmu->gid_info.valid =
			(*(u32 *)gid_data.signature == PMU_SHA1_GID_SIGNATURE);

		if (pmu->gid_info.valid) {

			BUG_ON(sizeof(pmu->gid_info.gid) !=
				sizeof(gid_data.gid));

			memcpy(pmu->gid_info.gid, gid_data.gid,
				sizeof(pmu->gid_info.gid));
		}
	}

	init = &msg->msg.init.pmu_init;
	for (i = 0; i < PMU_QUEUE_COUNT; i++)
		pmu_queue_init(&pmu->queue[i], i, init);

	nvhost_allocator_init(&pmu->dmem, "gk20a_pmu_dmem",
			msg->msg.init.pmu_init.sw_managed_area_offset,
			msg->msg.init.pmu_init.sw_managed_area_size,
			PMU_DMEM_ALLOC_ALIGNMENT);

	pmu->pmu_ready = true;

	return 0;
}

static bool pmu_read_message(struct pmu_gk20a *pmu, struct pmu_queue *queue,
			struct pmu_msg *msg, int *status)
{
	struct gk20a *g = pmu->g;
	u32 read_size, bytes_read;
	int err;

	*status = 0;

	if (pmu_queue_is_empty(pmu, queue))
		return false;

	err = pmu_queue_open_read(pmu, queue);
	if (err) {
		nvhost_err(dev_from_gk20a(g),
			"fail to open queue %d for read", queue->id);
		*status = err;
		return false;
	}

	err = pmu_queue_pop(pmu, queue, &msg->hdr,
			PMU_MSG_HDR_SIZE, &bytes_read);
	if (err || bytes_read != PMU_MSG_HDR_SIZE) {
		nvhost_err(dev_from_gk20a(g),
			"fail to read msg from queue %d", queue->id);
		*status = err | -EINVAL;
		goto clean_up;
	}

	if (msg->hdr.unit_id == PMU_UNIT_REWIND) {
		pmu_queue_rewind(pmu, queue);
		/* read again after rewind */
		err = pmu_queue_pop(pmu, queue, &msg->hdr,
				PMU_MSG_HDR_SIZE, &bytes_read);
		if (err || bytes_read != PMU_MSG_HDR_SIZE) {
			nvhost_err(dev_from_gk20a(g),
				"fail to read msg from queue %d", queue->id);
			*status = err | -EINVAL;
			goto clean_up;
		}
	}

	if (!PMU_UNIT_ID_IS_VALID(msg->hdr.unit_id)) {
		nvhost_err(dev_from_gk20a(g),
			"read invalid unit_id %d from queue %d",
			msg->hdr.unit_id, queue->id);
			*status = -EINVAL;
			goto clean_up;
	}

	if (msg->hdr.size > PMU_MSG_HDR_SIZE) {
		read_size = msg->hdr.size - PMU_MSG_HDR_SIZE;
		err = pmu_queue_pop(pmu, queue, &msg->msg,
			read_size, &bytes_read);
		if (err || bytes_read != read_size) {
			nvhost_err(dev_from_gk20a(g),
				"fail to read msg from queue %d", queue->id);
			*status = err;
			goto clean_up;
		}
	}

	err = pmu_queue_close(pmu, queue, true);
	if (err) {
		nvhost_err(dev_from_gk20a(g),
			"fail to close queue %d", queue->id);
		*status = err;
		return false;
	}

	return true;

clean_up:
	err = pmu_queue_close(pmu, queue, false);
	if (err)
		nvhost_err(dev_from_gk20a(g),
			"fail to close queue %d", queue->id);
	return false;
}

static int pmu_response_handle(struct pmu_gk20a *pmu,
			struct pmu_msg *msg)
{
	struct gk20a *g = pmu->g;
	struct pmu_sequence *seq;
	int ret = 0;

	nvhost_dbg_fn("");

	seq = &pmu->seq[msg->hdr.seq_id];
	if (seq->state != PMU_SEQ_STATE_USED &&
	    seq->state != PMU_SEQ_STATE_CANCELLED) {
		nvhost_err(dev_from_gk20a(g),
			"msg for an unknown sequence %d", seq->id);
		return -EINVAL;
	}

	if (msg->hdr.unit_id == PMU_UNIT_RC &&
	    msg->msg.rc.msg_type == PMU_RC_MSG_TYPE_UNHANDLED_CMD) {
		nvhost_err(dev_from_gk20a(g),
			"unhandled cmd: seq %d", seq->id);
	}
	else if (seq->state != PMU_SEQ_STATE_CANCELLED) {
		if (seq->msg) {
			if (seq->msg->hdr.size >= msg->hdr.size) {
				memcpy(seq->msg, msg, msg->hdr.size);
				if (seq->out.size != 0) {
					pmu_copy_from_dmem(pmu,
						seq->out.offset,
						seq->out_payload,
						seq->out.size,
						0);
				}
			} else {
				nvhost_err(dev_from_gk20a(g),
					"sequence %d msg buffer too small",
					seq->id);
			}
		}
	} else
		seq->callback = NULL;

	if (seq->in.size != 0)
		pmu->dmem.free(&pmu->dmem, seq->in.offset, seq->in.size);
	if (seq->out.size != 0)
		pmu->dmem.free(&pmu->dmem, seq->out.offset, seq->out.size);

	if (seq->callback)
		seq->callback(g, msg, seq->cb_params, seq->desc, ret);

	pmu_seq_release(pmu, seq);

	/* TBD: notify client waiting for available dmem */

	return 0;
}

static int pmu_process_message(struct pmu_gk20a *pmu)
{
	struct pmu_msg msg;
	int status;

	if (unlikely(!pmu->pmu_ready)) {
		pmu_process_init_msg(pmu, &msg);
		pmu_init_powergating(pmu);
		return 0;
	}

	while (pmu_read_message(pmu,
		&pmu->queue[PMU_MESSAGE_QUEUE], &msg, &status)) {

		nvhost_dbg_pmu("read msg hdr: "
				"unit_id = 0x%08x, size = 0x%08x, "
				"ctrl_flags = 0x%08x, seq_id = 0x%08x",
				msg.hdr.unit_id, msg.hdr.size,
				msg.hdr.ctrl_flags, msg.hdr.seq_id);

		msg.hdr.ctrl_flags &= ~PMU_CMD_FLAGS_PMU_MASK;

		if (msg.hdr.ctrl_flags == PMU_CMD_FLAGS_EVENT) {
			/* TBD: handle event callbacks */
		} else {
			pmu_response_handle(pmu, &msg);
		}
	}

	return 0;
}

void gk20a_pmu_isr(struct gk20a *g)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct pmu_queue *queue;
	u32 intr, mask;
	bool recheck = false;

	nvhost_dbg_fn("");

	mask = gk20a_readl(g, pwr_falcon_irqmask_r()) &
		gk20a_readl(g, pwr_falcon_irqdest_r());

	intr = gk20a_readl(g, pwr_falcon_irqstat_r()) & mask;

	nvhost_dbg_pmu("received falcon interrupt: 0x%08x", intr);

	if (!intr)
		return;

	if (intr & pwr_falcon_irqstat_halt_true_f()) {
		nvhost_err(dev_from_gk20a(g),
			"pmu halt intr not implemented");
	}
	if (intr & pwr_falcon_irqstat_exterr_true_f()) {
		nvhost_err(dev_from_gk20a(g),
			"pmu exterr intr not implemented");
	}
	if (intr & pwr_falcon_irqstat_swgen0_true_f()) {
		pmu_process_message(pmu);
		recheck = true;
	}

	gk20a_writel(g, pwr_falcon_irqsclr_r(), intr);

	if (recheck) {
		queue = &pmu->queue[PMU_MESSAGE_QUEUE];
		if (!pmu_queue_is_empty(pmu, queue))
			gk20a_writel(g, pwr_falcon_irqsset_r(),
				pwr_falcon_irqsset_swgen0_set_f());
	}
}

static bool pmu_validate_cmd(struct pmu_gk20a *pmu, struct pmu_cmd *cmd,
			struct pmu_msg *msg, struct pmu_payload *payload,
			u32 queue_id)
{
	struct gk20a *g = pmu->g;
	struct pmu_queue *queue;
	u32 in_size, out_size;

	if (!PMU_IS_SW_COMMAND_QUEUE(queue_id))
		goto invalid_cmd;

	queue = &pmu->queue[queue_id];
	if (cmd->hdr.size < PMU_CMD_HDR_SIZE)
		goto invalid_cmd;

	if (cmd->hdr.size > (queue->size >> 1))
		goto invalid_cmd;

	if (msg != NULL && msg->hdr.size < PMU_MSG_HDR_SIZE)
		goto invalid_cmd;

	if (!PMU_UNIT_ID_IS_VALID(cmd->hdr.unit_id))
		goto invalid_cmd;

	if (payload == NULL)
		return true;

	if (payload->in.buf == NULL && payload->out.buf == NULL)
		goto invalid_cmd;

	if ((payload->in.buf != NULL && payload->in.size == 0) ||
	    (payload->out.buf != NULL && payload->out.size == 0))
		goto invalid_cmd;

	in_size = PMU_CMD_HDR_SIZE;
	if (payload->in.buf) {
		in_size += payload->in.offset;
		in_size += sizeof(struct pmu_allocation);
	}

	out_size = PMU_CMD_HDR_SIZE;
	if (payload->out.buf) {
		out_size += payload->out.offset;
		out_size += sizeof(struct pmu_allocation);
	}

	if (in_size > cmd->hdr.size || out_size > cmd->hdr.size)
		goto invalid_cmd;


	if ((payload->in.offset != 0 && payload->in.buf == NULL) ||
	    (payload->out.offset != 0 && payload->out.buf == NULL))
		goto invalid_cmd;

	return true;

invalid_cmd:
	nvhost_err(dev_from_gk20a(g), "invalid pmu cmd :\n"
		"queue_id=%d,\n"
		"cmd_size=%d, cmd_unit_id=%d, msg=%p, msg_size=%d,\n"
		"payload in=%p, in_size=%d, in_offset=%d,\n"
		"payload out=%p, out_size=%d, out_offset=%d",
		queue_id, cmd->hdr.size, cmd->hdr.unit_id,
		msg, msg?msg->hdr.unit_id:~0,
		&payload->in, payload->in.size, payload->in.offset,
		&payload->out, payload->out.size, payload->out.offset);

	return false;
}

static int pmu_write_cmd(struct pmu_gk20a *pmu, struct pmu_cmd *cmd,
			u32 queue_id, u32 timeout)
{
	struct gk20a *g = pmu->g;
	struct pmu_queue *queue;
	int err;

	nvhost_dbg_fn("");

	queue = &pmu->queue[queue_id];

	do {
		err = pmu_queue_open_write(pmu, queue, cmd->hdr.size);
		if (err == -EAGAIN && timeout >= 0) {
			timeout--;
			msleep(1);
		} else
			break;
	}
	while (1);

	if (err)
		goto clean_up;

	pmu_queue_push(pmu, queue, cmd, cmd->hdr.size);

	err = pmu_queue_close(pmu, queue, true);

clean_up:
	if (err)
		nvhost_err(dev_from_gk20a(g),
			"fail to write cmd to queue %d", queue_id);
	else
		nvhost_dbg_fn("done");

	return err;
}

int gk20a_pmu_cmd_post(struct gk20a *g, struct pmu_cmd *cmd,
		struct pmu_msg *msg, struct pmu_payload *payload,
		u32 queue_id, pmu_callback callback, void* cb_param,
		u32 *seq_desc, u32 timeout)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct pmu_sequence *seq;
	struct pmu_allocation *in = NULL, *out = NULL;
	int err;

	nvhost_dbg_fn("");

	BUG_ON(!cmd);
	BUG_ON(!seq_desc);
	BUG_ON(!pmu->pmu_ready);

	if (!pmu_validate_cmd(pmu, cmd, msg, payload, queue_id))
		return -EINVAL;

	err = pmu_seq_acquire(pmu, &seq);
	if (err)
		return err;

	cmd->hdr.seq_id = seq->id;

	cmd->hdr.ctrl_flags = 0;
	cmd->hdr.ctrl_flags |= PMU_CMD_FLAGS_STATUS;
	cmd->hdr.ctrl_flags |= PMU_CMD_FLAGS_INTR;

	seq->callback = callback;
	seq->cb_params = cb_param;
	seq->msg = msg;
	seq->out_payload = NULL;
	seq->desc = pmu->next_seq_desc++;

	if (payload)
		seq->out_payload = payload->out.buf;

	*seq_desc = seq->desc;

	if (payload && payload->in.offset != 0) {
		in = (struct pmu_allocation *)
			((u8 *)&cmd->cmd + payload->in.offset);

		if (payload->in.buf != payload->out.buf)
			in->size = (u16)payload->in.size;
		else
			in->size = (u16)max(payload->in.size,
				payload->out.size);

		err = pmu->dmem.alloc(&pmu->dmem, &in->offset, in->size);
		if (err)
			goto clean_up;

		pmu_copy_to_dmem(pmu, in->offset,
			payload->in.buf, payload->in.size, 0);

		seq->in.size = in->size;
		seq->in.offset = in->offset;
	}

	if (payload && payload->out.offset != 0) {
		out = (struct pmu_allocation *)
			((u8 *)&cmd->cmd + payload->out.offset);

		out->size = (u16)payload->out.size;

		if (payload->out.buf != payload->in.buf) {
			err = pmu->dmem.alloc(&pmu->dmem,
				&out->offset, out->size);
			if (err)
				goto clean_up;
		} else {
			BUG_ON(in == NULL);
			out->offset = in->offset;
		}

		seq->out.size = out->size;
		seq->out.offset = out->offset;
	}

	err = pmu_write_cmd(pmu, cmd, queue_id, timeout);
	if (!err)
		seq->state = PMU_SEQ_STATE_USED;

	nvhost_dbg_fn("done");

	if (0) {
		struct pmu_pg_stats stats;
		u32 i, val[20];

		pmu_copy_from_dmem(pmu, pmu->stat_dmem_offset,
			(u8 *)&stats, sizeof(struct pmu_pg_stats), 0);

		nvhost_dbg_pmu("pg_entry_start_timestamp : 0x%016llx",
			stats.pg_entry_start_timestamp);
		nvhost_dbg_pmu("pg_exit_start_timestamp : 0x%016llx",
			stats.pg_exit_start_timestamp);
		nvhost_dbg_pmu("pg_ingating_start_timestamp : 0x%016llx",
			stats.pg_ingating_start_timestamp);
		nvhost_dbg_pmu("pg_ungating_start_timestamp : 0x%016llx",
			stats.pg_ungating_start_timestamp);
		nvhost_dbg_pmu("pg_avg_entry_time_us : 0x%08x",
			stats.pg_avg_entry_time_us);
		nvhost_dbg_pmu("pg_avg_exit_time_us : 0x%08x",
			stats.pg_avg_exit_time_us);
		nvhost_dbg_pmu("pg_ingating_cnt : 0x%08x",
			stats.pg_ingating_cnt);
		nvhost_dbg_pmu("pg_ingating_time_us : 0x%08x",
			stats.pg_ingating_time_us);
		nvhost_dbg_pmu("pg_ungating_count : 0x%08x",
			stats.pg_ungating_count);
		nvhost_dbg_pmu("pg_ungating_time_us 0x%08x: ",
			stats.pg_ungating_time_us);
		nvhost_dbg_pmu("pg_gating_cnt : 0x%08x",
			stats.pg_gating_cnt);
		nvhost_dbg_pmu("pg_gating_deny_cnt : 0x%08x",
			stats.pg_gating_deny_cnt);

		/* symbol "ElpgLog" offset 0x1000066c in ucode .nm file */
		pmu_copy_from_dmem(pmu, 0x66c,
			(u8 *)val, sizeof(val), 0);
		nvhost_dbg_pmu("elpg log begin");
		for (i = 0; i < 20; i++)
			nvhost_dbg_pmu("0x%08x", val[i]);
		nvhost_dbg_pmu("elpg log end");

		i = gk20a_readl(g, pwr_pmu_idle_mask_supp_r(3));
		nvhost_dbg_pmu("pwr_pmu_idle_mask_supp_r(3): 0x%08x", i);
		i = gk20a_readl(g, pwr_pmu_idle_mask_1_supp_r(3));
		nvhost_dbg_pmu("pwr_pmu_idle_mask_1_supp_r(3): 0x%08x", i);
		i = gk20a_readl(g, pwr_pmu_idle_ctrl_supp_r(3));
		nvhost_dbg_pmu("pwr_pmu_idle_ctrl_supp_r(3): 0x%08x", i);
		i = gk20a_readl(g, pwr_pmu_pg_idle_cnt_r(0));
		nvhost_dbg_pmu("pwr_pmu_pg_idle_cnt_r(0): 0x%08x", i);
		i = gk20a_readl(g, pwr_pmu_pg_intren_r(0));
		nvhost_dbg_pmu("pwr_pmu_pg_intren_r(0): 0x%08x", i);

		/* TBD: script can't generate those registers correctly
		i = gk20a_readl(g, pwr_pmu_idle_status_r());
		nvhost_dbg_pmu("pwr_pmu_idle_status_r(): 0x%08x", i);
		i = gk20a_readl(g, pwr_pmu_pg_ctrl_r());
		nvhost_dbg_pmu("pwr_pmu_pg_ctrl_r(): 0x%08x", i);
		*/
	}

	return 0;

clean_up:
	nvhost_dbg_fn("fail");
	if (in)
		pmu->dmem.free(&pmu->dmem, in->offset, in->size);
	if (out)
		pmu->dmem.free(&pmu->dmem, out->offset, out->size);

	pmu_seq_release(pmu, seq);
	return err;
}

int gk20a_pmu_enable_elpg(struct gk20a *g)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct gr_gk20a *gr = &g->gr;
	struct pmu_cmd cmd;
	u32 seq;

	nvhost_dbg_fn("");

	if (!pmu->elpg_ready)
		return 0;

	/* do NOT enable elpg until golden ctx is created,
	   which is related with the ctx that ELPG save and restore. */
	if (unlikely(!gr->ctx_vars.golden_image_initialized))
		return 0;

	/* return if ELPG is already on or on_pending or off_on_pending */
	if (pmu->elpg_stat != PMU_ELPG_STAT_OFF)
		return 0;

	if (!pmu->elpg_enable_allow) {
		pmu->elpg_stat = PMU_ELPG_STAT_OFF_ON_PENDING;
		return 0;
	}

	memset(&cmd, 0, sizeof(struct pmu_cmd));
	cmd.hdr.unit_id = PMU_UNIT_PG;
	cmd.hdr.size = PMU_CMD_HDR_SIZE + sizeof(struct pmu_pg_cmd_elpg_cmd);
	cmd.cmd.pg.elpg_cmd.cmd_type = PMU_PG_CMD_TYPE_ELPG_CMD;
	cmd.cmd.pg.elpg_cmd.engine_id = ENGINE_GR_GK20A;
	cmd.cmd.pg.elpg_cmd.cmd = PMU_PG_ELPG_CMD_ALLOW;

	gk20a_pmu_cmd_post(g, &cmd, NULL, NULL, PMU_COMMAND_QUEUE_HPQ,
			pmu_handle_pg_elpg_msg, pmu, &seq, ~0);

	/* no need to wait ack for ELPG enable but set pending to sync
	   with follow up ELPG disable */
	pmu->elpg_stat = PMU_ELPG_STAT_ON_PENDING;

	nvhost_dbg_fn("done");

	return 0;
}

int gk20a_pmu_disable_elpg(struct gk20a *g)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct pmu_cmd cmd;
	u32 seq;
	int remain;

	nvhost_dbg_fn("");

	if (!pmu->elpg_ready)
		return 0;

	/* cancel off_on_pending and return */
	if (pmu->elpg_stat == PMU_ELPG_STAT_OFF_ON_PENDING) {
		pmu->elpg_stat = PMU_ELPG_STAT_OFF;
		return 0;
	}
	/* wait if on_pending */
	else if (pmu->elpg_stat == PMU_ELPG_STAT_ON_PENDING) {
		remain = wait_event_interruptible_timeout(
			pmu->pg_wq,
			pmu->elpg_stat == PMU_ELPG_STAT_ON,
			2 * HZ /* 2 sec */);
		if (pmu->elpg_stat != PMU_ELPG_STAT_ON) {
			nvhost_err(dev_from_gk20a(g),
				"ELPG_ALLOW_ACK failed, remaining timeout 0x%08x",
				remain);
			return -EBUSY;
		}
	}
	/* return if ELPG is already off */
	else if (pmu->elpg_stat != PMU_ELPG_STAT_ON)
		return 0;

	memset(&cmd, 0, sizeof(struct pmu_cmd));
	cmd.hdr.unit_id = PMU_UNIT_PG;
	cmd.hdr.size = PMU_CMD_HDR_SIZE + sizeof(struct pmu_pg_cmd_elpg_cmd);
	cmd.cmd.pg.elpg_cmd.cmd_type = PMU_PG_CMD_TYPE_ELPG_CMD;
	cmd.cmd.pg.elpg_cmd.engine_id = ENGINE_GR_GK20A;
	cmd.cmd.pg.elpg_cmd.cmd = PMU_PG_ELPG_CMD_DISALLOW;

	gk20a_pmu_cmd_post(g, &cmd, NULL, NULL, PMU_COMMAND_QUEUE_HPQ,
			pmu_handle_pg_elpg_msg, pmu, &seq, ~0);

	remain = wait_event_interruptible_timeout(
			pmu->pg_wq,
			pmu->elpg_stat == PMU_ELPG_STAT_OFF,
			2 * HZ /* 2 sec */);
	if (pmu->elpg_stat != PMU_ELPG_STAT_OFF) {
		nvhost_err(dev_from_gk20a(g),
			"ELPG_DISALLOW_ACK failed, remaining timeout 0x%08x",
			remain);
		return -EBUSY;
	}

	if (!timer_pending(&pmu->elpg_timer)) {
		pmu->elpg_enable_allow = false;
		pmu->elpg_timer.expires = jiffies +
			msecs_to_jiffies(PMU_ELPG_ENABLE_ALLOW_DELAY_MSEC);
		add_timer(&pmu->elpg_timer);
	}

	nvhost_dbg_fn("done");

	return 0;
}