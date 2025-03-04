/*
 * Intel(R) Trace Hub Memory Storage Unit
 *
 * Copyright (C) 2014-2015 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/sizes.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>

#ifdef CONFIG_X86
#include <asm/set_memory.h>
#endif

#include <linux/acpi.h>
#include <linux/debugfs.h>

#include "intel_th.h"
#include "msu.h"

#define msc_dev(x) (&(x)->thdev->dev)

/**
 * struct msc_block - multiblock mode block descriptor
 * @bdesc:	pointer to hardware descriptor (beginning of the block)
 * @addr:	physical address of the block
 */
struct msc_block {
	struct msc_block_desc	*bdesc;
	dma_addr_t		addr;
};

/**
 * struct msc_window - multiblock mode window descriptor
 * @entry:	window list linkage (msc::win_list)
 * @pgoff:	page offset into the buffer that this window starts at
 * @nr_blocks:	number of blocks (pages) in this window
 * @block:	array of block descriptors
 */
struct msc_window {
	struct list_head	entry;
	unsigned long		pgoff;
	unsigned int		nr_blocks;
	struct msc		*msc;
	struct msc_block	block[0];
};

/**
 * struct msc_iter - iterator for msc buffer
 * @entry:		msc::iter_list linkage
 * @msc:		pointer to the MSC device
 * @start_win:		oldest window
 * @win:		current window
 * @offset:		current logical offset into the buffer
 * @start_block:	oldest block in the window
 * @block:		block number in the window
 * @block_off:		offset into current block
 * @wrap_count:		block wrapping handling
 * @eof:		end of buffer reached
 */
struct msc_iter {
	struct list_head	entry;
	struct msc		*msc;
	struct msc_window	*start_win;
	struct msc_window	*win;
	unsigned long		offset;
	int			start_block;
	int			block;
	unsigned int		block_off;
	unsigned int		wrap_count;
	unsigned int		eof;
};

/**
 * struct msc - MSC device representation
 * @reg_base:		register window base address
 * @thdev:		intel_th_device pointer
 * @win_list:		list of windows in multiblock mode
 * @nr_pages:		total number of pages allocated for this buffer
 * @single_sz:		amount of data in single mode
 * @single_wrap:	single mode wrap occurred
 * @base:		buffer's base pointer
 * @base_addr:		buffer's base address
 * @nwsa:		next window start address backup
 * @user_count:		number of users of the buffer
 * @mmap_count:		number of mappings
 * @buf_mutex:		mutex to serialize access to buffer-related bits

 * @enabled:		MSC is enabled
 * @wrap:		wrapping is enabled
 * @mode:		MSC operating mode
 * @burst_len:		write burst length
 * @index:		number of this MSC in the MSU
 *
 * @max_blocks:		Maximum number of blocks in a window
 */
struct msc {
	void __iomem		*reg_base;
	struct intel_th_device	*thdev;

	struct list_head	win_list;
	unsigned long		nr_pages;
	unsigned long		single_sz;
	unsigned int		single_wrap : 1;
	void			*base;
	dma_addr_t		base_addr;
	unsigned long		nwsa;

	/* <0: no buffer, 0: no users, >0: active users */
	atomic_t		user_count;

	atomic_t		mmap_count;
	struct mutex		buf_mutex;

	struct list_head	iter_list;

	/* config */
	unsigned int		enabled : 1,
				wrap	: 1;
	unsigned int		mode;
	unsigned int		burst_len;
	unsigned int		index;
	unsigned int		max_blocks;
};

static struct msc_probe_rem_cb msc_probe_rem_cb;

struct msc_device_instance {
	struct list_head list;
	struct intel_th_device *thdev;
};

static LIST_HEAD(msc_dev_instances);
static DEFINE_MUTEX(msc_dev_reg_lock);
/**
 * msc_register_callbacks()
 * @cbs
 */
int msc_register_callbacks(struct msc_probe_rem_cb cbs)
{
	struct msc_device_instance *it;

	mutex_lock(&msc_dev_reg_lock);

	msc_probe_rem_cb.probe = cbs.probe;
	msc_probe_rem_cb.remove = cbs.remove;
	/* Call the probe callback for the already existing ones*/
	list_for_each_entry(it, &msc_dev_instances, list) {
		cbs.probe(it->thdev);
	}

	mutex_unlock(&msc_dev_reg_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(msc_register_callbacks);

/**
 * msc_unregister_callbacks()
 */
void msc_unregister_callbacks(void)
{
	mutex_lock(&msc_dev_reg_lock);

	msc_probe_rem_cb.probe = NULL;
	msc_probe_rem_cb.remove = NULL;

	mutex_unlock(&msc_dev_reg_lock);
}
EXPORT_SYMBOL_GPL(msc_unregister_callbacks);

static void msc_add_instance(struct intel_th_device *thdev)
{
	struct msc_device_instance *instance;

	instance = kmalloc(sizeof(*instance), GFP_KERNEL);
	if (!instance)
		return;

	mutex_lock(&msc_dev_reg_lock);

	instance->thdev = thdev;
	list_add(&instance->list, &msc_dev_instances);

	if (msc_probe_rem_cb.probe)
		msc_probe_rem_cb.probe(thdev);

	mutex_unlock(&msc_dev_reg_lock);
}

static void msc_rm_instance(struct intel_th_device *thdev)
{
	struct msc_device_instance *instance = NULL, *it;

	mutex_lock(&msc_dev_reg_lock);

	if (msc_probe_rem_cb.remove)
		msc_probe_rem_cb.remove(thdev);

	list_for_each_entry(it, &msc_dev_instances, list) {
		if (it->thdev == thdev) {
			instance = it;
			break;
		}
	}

	if (instance) {
		list_del(&instance->list);
		kfree(instance);
	} else {
		pr_warn("msu: cannot remove %p (not found)", thdev);
	}

	mutex_unlock(&msc_dev_reg_lock);
}

static inline bool msc_block_is_empty(struct msc_block_desc *bdesc)
{
	/* header hasn't been written */
	if (!bdesc->valid_dw)
		return true;

	/* valid_dw includes the header */
	if (!msc_data_sz(bdesc))
		return true;

	return false;
}

/**
 * msc_current_window() - locate the window in use
 * @msc:	MSC device
 *
 * This should only be used in multiblock mode. Caller should hold the
 * msc::user_count reference.
 *
 * Return:	the current output window
 */
static struct msc_window *msc_current_window(struct msc *msc)
{
	struct msc_window *win, *prev = NULL;
	/*BAR is never changing, so the current one is the one before the next*/
	u32 reg = ioread32(msc->reg_base + REG_MSU_MSC0NWSA);
	unsigned long win_addr = (unsigned long)reg << PAGE_SHIFT;

	if (list_empty(&msc->win_list))
		return NULL;

	list_for_each_entry(win, &msc->win_list, entry) {
		if (win->block[0].addr == win_addr)
			break;
		prev = win;
	}
	if (!prev)
		prev = list_entry(msc->win_list.prev, struct msc_window, entry);

	return prev;
}


/**
 * msc_oldest_window() - locate the window with oldest data
 * @msc:	MSC device
 *
 * This should only be used in multiblock mode. Caller should hold the
 * msc::user_count reference.
 *
 * Return:	the oldest window with valid data
 */
static struct msc_window *msc_oldest_window(struct msc *msc)
{
	struct msc_window *win;
	unsigned int found = 0;
	unsigned long nwsa;

	if (list_empty(&msc->win_list))
		return NULL;

	if (msc->enabled) {
		u32 reg = ioread32(msc->reg_base + REG_MSU_MSC0NWSA);

		nwsa = (unsigned long)reg << PAGE_SHIFT;
	} else {
		nwsa = msc->nwsa;
	}
	/*
	 * we might need a radix tree for this, depending on how
	 * many windows a typical user would allocate; ideally it's
	 * something like 2, in which case we're good
	 */
	list_for_each_entry(win, &msc->win_list, entry) {
		if (win->block[0].addr == nwsa)
			found++;

		/* skip the empty ones */
		if (msc_block_is_empty(win->block[0].bdesc))
			continue;

		if (found)
			return win;
	}

	return list_entry(msc->win_list.next, struct msc_window, entry);
}

/**
 * msc_win_oldest_block() - locate the oldest block in a given window
 * @win:	window to look at
 *
 * Return:	index of the block with the oldest data
 */
static unsigned int msc_win_oldest_block(struct msc_window *win)
{
	unsigned int blk;
	struct msc_block_desc *bdesc = win->block[0].bdesc;

	/* without wrapping, first block is the oldest */
	if (!msc_block_wrapped(bdesc))
		return 0;

	/*
	 * with wrapping, last written block contains both the newest and the
	 * oldest data for this window.
	 */
	for (blk = 0; blk < win->nr_blocks; blk++) {
		bdesc = win->block[blk].bdesc;

		if (msc_block_last_written(bdesc))
			return blk;
	}

	return 0;
}

/**
 * msc_max_blocks() - get the maximum number of block
 * @thdev:	the sub-device
 *
 * Return:	the maximum number of blocks / window
 */
unsigned int msc_max_blocks(struct intel_th_device *thdev)
{
	struct msc *msc = dev_get_drvdata(&thdev->dev);

	return msc->max_blocks;
}
EXPORT_SYMBOL_GPL(msc_max_blocks);

/**
 * msc_block_max_size() - get the size of biggest block
 * @thdev:	the sub-device
 *
 * Return:	the size of biggest block
 */
unsigned int msc_block_max_size(struct intel_th_device *thdev)
{
	return PAGE_SIZE;
}
EXPORT_SYMBOL_GPL(msc_block_max_size);

/**
 * msc_switch_window() - perform a window switch
 * @thdev:	the sub-device
 */
int msc_switch_window(struct intel_th_device *thdev)
{
	intel_th_trace_switch(thdev);
	return 0;
}
EXPORT_SYMBOL_GPL(msc_switch_window);

/**
 * msc_current_win_bytes() - get the current window data size
 * @thdev:	the sub-device
 *
 * Get the number of valid data bytes in the current window.
 * Based on this the dvc-source part can decide to request a window switch.
 */
int msc_current_win_bytes(struct intel_th_device *thdev)
{
	struct msc *msc = dev_get_drvdata(&thdev->dev);
	struct msc_window *win;
	u32 reg_mwp, blk, offset, i;
	int size = 0;

	/* proceed only if actively storing in muli-window mode */
	if (!msc->enabled ||
	    (msc->mode != MSC_MODE_MULTI) ||
	    !atomic_inc_unless_negative(&msc->user_count))
		return -EINVAL;

	win = msc_current_window(msc);
	reg_mwp = ioread32(msc->reg_base + REG_MSU_MSC0MWP);

	if (!win) {
		atomic_dec(&msc->user_count);
		return -EINVAL;
	}

	blk = 0;
	while (blk < win->nr_blocks) {
		if (win->block[blk].addr == (reg_mwp & PAGE_MASK))
			break;
		blk++;
	}

	if (blk >= win->nr_blocks) {
		atomic_dec(&msc->user_count);
		return -EINVAL;
	}

	offset = (reg_mwp & (PAGE_SIZE - 1));


	/*if wrap*/
	if (msc_block_wrapped(win->block[blk].bdesc)) {
		for (i = blk+1; i < win->nr_blocks; i++)
			size += msc_data_sz(win->block[i].bdesc);
	}

	for (i = 0; i < blk; i++)
		size += msc_data_sz(win->block[i].bdesc);

	/*finaly the current one*/
	size += (offset - MSC_BDESC);

	atomic_dec(&msc->user_count);
	return size;
}
EXPORT_SYMBOL_GPL(msc_current_win_bytes);

/**
 * msc_sg_oldest_win() - get the data from the oldest window
 * @thdev:	the sub-device
 * @sg_array:	destination sg array
 *
 * Return:	sg count
 */
int msc_sg_oldest_win(struct intel_th_device *thdev,
		      struct scatterlist *sg_array)
{
	struct msc *msc = dev_get_drvdata(&thdev->dev);
	struct msc_window *win, *c_win;
	struct msc_block_desc *bdesc;
	unsigned int blk, sg = 0;

	/* proceed only if actively storing in muli-window mode */
	if (!msc->enabled ||
	    (msc->mode != MSC_MODE_MULTI) ||
	    !atomic_inc_unless_negative(&msc->user_count))
		return -EINVAL;

	win = msc_oldest_window(msc);
	if (!win)
		return 0;

	c_win = msc_current_window(msc);

	if (win == c_win)
		return 0;

	blk = msc_win_oldest_block(win);

	/* start with the first block containing only oldest data */
	if (msc_block_wrapped(win->block[blk].bdesc))
		if (++blk == win->nr_blocks)
			blk = 0;

	do {
		bdesc = win->block[blk].bdesc;
		sg_set_buf(&sg_array[sg++], bdesc, PAGE_SIZE);

		if (bdesc->hw_tag & MSC_HW_TAG_ENDBIT)
			break;

		if (++blk == win->nr_blocks)
			blk = 0;

	} while (sg <= win->nr_blocks);

	sg_mark_end(&sg_array[sg - 1]);

	atomic_dec(&msc->user_count);

	return sg;
}
EXPORT_SYMBOL_GPL(msc_sg_oldest_win);

/**
 * msc_is_last_win() - check if a window is the last one for a given MSC
 * @win:	window
 * Return:	true if @win is the last window in MSC's multiblock buffer
 */
static inline bool msc_is_last_win(struct msc_window *win)
{
	return win->entry.next == &win->msc->win_list;
}

/**
 * msc_next_window() - return next window in the multiblock buffer
 * @win:	current window
 *
 * Return:	window following the current one
 */
static struct msc_window *msc_next_window(struct msc_window *win)
{
	if (msc_is_last_win(win))
		return list_entry(win->msc->win_list.next, struct msc_window,
				  entry);

	return list_entry(win->entry.next, struct msc_window, entry);
}

static struct msc_block_desc *msc_iter_bdesc(struct msc_iter *iter)
{
	return iter->win->block[iter->block].bdesc;
}

static void msc_iter_init(struct msc_iter *iter)
{
	memset(iter, 0, sizeof(*iter));
	iter->start_block = -1;
	iter->block = -1;
}

static struct msc_iter *msc_iter_install(struct msc *msc)
{
	struct msc_iter *iter;

	iter = kzalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&msc->buf_mutex);

	/*
	 * Reading and tracing are mutually exclusive; if msc is
	 * enabled, open() will fail; otherwise existing readers
	 * will prevent enabling the msc and the rest of fops don't
	 * need to worry about it.
	 */
	if (msc->enabled) {
		kfree(iter);
		iter = ERR_PTR(-EBUSY);
		goto unlock;
	}

	msc_iter_init(iter);
	iter->msc = msc;

	list_add_tail(&iter->entry, &msc->iter_list);
unlock:
	mutex_unlock(&msc->buf_mutex);

	return iter;
}

static void msc_iter_remove(struct msc_iter *iter, struct msc *msc)
{
	mutex_lock(&msc->buf_mutex);
	list_del(&iter->entry);
	mutex_unlock(&msc->buf_mutex);

	kfree(iter);
}

static void msc_iter_block_start(struct msc_iter *iter)
{
	if (iter->start_block != -1)
		return;

	iter->start_block = msc_win_oldest_block(iter->win);
	iter->block = iter->start_block;
	iter->wrap_count = 0;

	/*
	 * start with the block with oldest data; if data has wrapped
	 * in this window, it should be in this block
	 */
	if (msc_block_wrapped(msc_iter_bdesc(iter)))
		iter->wrap_count = 2;

}

static int msc_iter_win_start(struct msc_iter *iter, struct msc *msc)
{
	/* already started, nothing to do */
	if (iter->start_win)
		return 0;

	iter->start_win = msc_oldest_window(msc);
	if (!iter->start_win)
		return -EINVAL;

	iter->win = iter->start_win;
	iter->start_block = -1;

	msc_iter_block_start(iter);

	return 0;
}

static int msc_iter_win_advance(struct msc_iter *iter)
{
	iter->win = msc_next_window(iter->win);
	iter->start_block = -1;

	if (iter->win == iter->start_win) {
		iter->eof++;
		return 1;
	}

	msc_iter_block_start(iter);

	return 0;
}

static int msc_iter_block_advance(struct msc_iter *iter)
{
	iter->block_off = 0;

	/* wrapping */
	if (iter->wrap_count && iter->block == iter->start_block) {
		iter->wrap_count--;
		if (!iter->wrap_count)
			/* copied newest data from the wrapped block */
			return msc_iter_win_advance(iter);
	}

	/* no wrapping, check for last written block */
	if (!iter->wrap_count && msc_block_last_written(msc_iter_bdesc(iter)))
		/* copied newest data for the window */
		return msc_iter_win_advance(iter);

	/* block advance */
	if (++iter->block == iter->win->nr_blocks)
		iter->block = 0;

	/* no wrapping, sanity check in case there is no last written block */
	if (!iter->wrap_count && iter->block == iter->start_block)
		return msc_iter_win_advance(iter);

	return 0;
}

/**
 * msc_buffer_iterate() - go through multiblock buffer's data
 * @iter:	iterator structure
 * @size:	amount of data to scan
 * @data:	callback's private data
 * @fn:		iterator callback
 *
 * This will start at the window which will be written to next (containing
 * the oldest data) and work its way to the current window, calling @fn
 * for each chunk of data as it goes.
 *
 * Caller should have msc::user_count reference to make sure the buffer
 * doesn't disappear from under us.
 *
 * Return:	amount of data actually scanned.
 */
static ssize_t
msc_buffer_iterate(struct msc_iter *iter, size_t size, void *data,
		   unsigned long (*fn)(void *, void *, size_t))
{
	struct msc *msc = iter->msc;
	size_t len = size;
	unsigned int advance;

	if (iter->eof)
		return 0;

	/* start with the oldest window */
	if (msc_iter_win_start(iter, msc))
		return 0;

	do {
		unsigned long data_bytes = msc_data_sz(msc_iter_bdesc(iter));
		void *src = (void *)msc_iter_bdesc(iter) + MSC_BDESC;
		size_t tocopy = data_bytes, copied = 0;
		size_t remaining = 0;

		advance = 1;

		/*
		 * If block wrapping happened, we need to visit the last block
		 * twice, because it contains both the oldest and the newest
		 * data in this window.
		 *
		 * First time (wrap_count==2), in the very beginning, to collect
		 * the oldest data, which is in the range
		 * (data_bytes..DATA_IN_PAGE).
		 *
		 * Second time (wrap_count==1), it's just like any other block,
		 * containing data in the range of [MSC_BDESC..data_bytes].
		 */
		if (iter->block == iter->start_block && iter->wrap_count == 2) {
			tocopy = DATA_IN_PAGE - data_bytes;
			src += data_bytes;
		}

		if (!tocopy)
			goto next_block;

		tocopy -= iter->block_off;
		src += iter->block_off;

		if (len < tocopy) {
			tocopy = len;
			advance = 0;
		}

		remaining = fn(data, src, tocopy);

		if (remaining)
			advance = 0;

		copied = tocopy - remaining;
		len -= copied;
		iter->block_off += copied;
		iter->offset += copied;

		if (!advance)
			break;

next_block:
		if (msc_iter_block_advance(iter))
			break;

	} while (len);

	return size - len;
}

/**
 * msc_buffer_clear_hw_header() - clear hw header for multiblock
 * @msc:	MSC device
 */
static void msc_buffer_clear_hw_header(struct msc *msc)
{
	struct msc_window *win;

	list_for_each_entry(win, &msc->win_list, entry) {
		unsigned int blk;
		size_t hw_sz = sizeof(struct msc_block_desc) -
			offsetof(struct msc_block_desc, hw_tag);

		for (blk = 0; blk < win->nr_blocks; blk++) {
			struct msc_block_desc *bdesc = win->block[blk].bdesc;

			memset(&bdesc->hw_tag, 0, hw_sz);
		}
	}
}

/**
 * msc_configure() - set up MSC hardware
 * @msc:	the MSC device to configure
 *
 * Program all relevant registers for a given MSC.
 * Programming registers must be delayed until this stage since the hardware
 * will be reset before a capture is started.
 */
static int msc_configure(struct msc *msc)
{
	u32 reg;

	lockdep_assert_held(&msc->buf_mutex);

	if (msc->mode > MSC_MODE_MULTI)
		return -ENOTSUPP;

	if (msc->mode == MSC_MODE_MULTI)
		msc_buffer_clear_hw_header(msc);

	reg = msc->base_addr >> PAGE_SHIFT;
	iowrite32(reg, msc->reg_base + REG_MSU_MSC0BAR);

	if (msc->mode == MSC_MODE_SINGLE) {
		reg = msc->nr_pages;
		iowrite32(reg, msc->reg_base + REG_MSU_MSC0SIZE);
	}

	reg = ioread32(msc->reg_base + REG_MSU_MSC0CTL);
	reg &= ~(MSC_MODE | MSC_WRAPEN | MSC_EN | MSC_RD_HDR_OVRD);

	reg |= MSC_EN;
	reg |= msc->mode << __ffs(MSC_MODE);
	reg |= msc->burst_len << __ffs(MSC_LEN);

	if (msc->wrap)
		reg |= MSC_WRAPEN;

	iowrite32(reg, msc->reg_base + REG_MSU_MSC0CTL);

	msc->thdev->output.multiblock = msc->mode == MSC_MODE_MULTI;
	msc->enabled = 1;

	return 0;
}

/**
 * msc_disable() - disable MSC hardware
 * @msc:	MSC device to disable
 *
 * If @msc is enabled, disable tracing on the switch and then disable MSC
 * storage. Caller must hold msc::buf_mutex.
 */
static void msc_disable(struct msc *msc)
{
	u32 reg;

	lockdep_assert_held(&msc->buf_mutex);

	intel_th_trace_disable(msc->thdev);

	if (msc->mode == MSC_MODE_SINGLE) {
		reg = ioread32(msc->reg_base + REG_MSU_MSC0STS);
		msc->single_wrap = !!(reg & MSCSTS_WRAPSTAT);

		reg = ioread32(msc->reg_base + REG_MSU_MSC0MWP);
		msc->single_sz = reg & ((msc->nr_pages << PAGE_SHIFT) - 1);
		dev_dbg(msc_dev(msc), "MSCnMWP: %08x/%08lx, wrap: %d\n",
			reg, msc->single_sz, msc->single_wrap);
	}

	/* Save next window start address before disabling */
	reg = ioread32(msc->reg_base + REG_MSU_MSC0NWSA);
	msc->nwsa = (unsigned long)reg << PAGE_SHIFT;

	reg = ioread32(msc->reg_base + REG_MSU_MSC0CTL);
	reg &= ~MSC_EN;
	iowrite32(reg, msc->reg_base + REG_MSU_MSC0CTL);
	msc->enabled = 0;

	iowrite32(0, msc->reg_base + REG_MSU_MSC0BAR);
	iowrite32(0, msc->reg_base + REG_MSU_MSC0SIZE);

	dev_dbg(msc_dev(msc), "MSCnNWSA: %08lx\n", msc->nwsa);

	reg = ioread32(msc->reg_base + REG_MSU_MSC0STS);
	dev_dbg(msc_dev(msc), "MSCnSTS: %08x\n", reg);
}

static int intel_th_msc_activate(struct intel_th_device *thdev)
{
	struct msc *msc = dev_get_drvdata(&thdev->dev);
	int ret = -EBUSY;

	if (!atomic_inc_unless_negative(&msc->user_count))
		return -ENODEV;

	mutex_lock(&msc->buf_mutex);

	/* if there are readers, refuse */
	if (list_empty(&msc->iter_list))
		ret = msc_configure(msc);

	mutex_unlock(&msc->buf_mutex);

	if (ret)
		atomic_dec(&msc->user_count);

	return ret;
}

static void intel_th_msc_deactivate(struct intel_th_device *thdev)
{
	struct msc *msc = dev_get_drvdata(&thdev->dev);

	mutex_lock(&msc->buf_mutex);
	if (msc->enabled) {
		msc_disable(msc);
		atomic_dec(&msc->user_count);
	}
	mutex_unlock(&msc->buf_mutex);
}

/**
 * msc_buffer_contig_alloc() - allocate a contiguous buffer for SINGLE mode
 * @msc:	MSC device
 * @size:	allocation size in bytes
 *
 * This modifies msc::base, which requires msc::buf_mutex to serialize, so the
 * caller is expected to hold it.
 *
 * Return:	0 on success, -errno otherwise.
 */
static int msc_buffer_contig_alloc(struct msc *msc, unsigned long size)
{
	unsigned int order = get_order(size);
	struct page *page;

	if (!size)
		return 0;

	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	split_page(page, order);
	msc->nr_pages = size >> PAGE_SHIFT;
	msc->base = page_address(page);
	msc->base_addr = page_to_phys(page);

	return 0;
}

/**
 * msc_buffer_contig_free() - free a contiguous buffer
 * @msc:	MSC configured in SINGLE mode
 */
static void msc_buffer_contig_free(struct msc *msc)
{
	unsigned long off;

	for (off = 0; off < msc->nr_pages << PAGE_SHIFT; off += PAGE_SIZE) {
		struct page *page = virt_to_page(msc->base + off);

		page->mapping = NULL;
		__free_page(page);
	}

	msc->nr_pages = 0;
}

/**
 * msc_buffer_contig_get_page() - find a page at a given offset
 * @msc:	MSC configured in SINGLE mode
 * @pgoff:	page offset
 *
 * Return:	page, if @pgoff is within the range, NULL otherwise.
 */
static struct page *msc_buffer_contig_get_page(struct msc *msc,
					       unsigned long pgoff)
{
	if (pgoff >= msc->nr_pages)
		return NULL;

	return virt_to_page(msc->base + (pgoff << PAGE_SHIFT));
}

/**
 * msc_buffer_win_alloc() - alloc a window for a multiblock mode
 * @msc:	MSC device
 * @nr_blocks:	number of pages in this window
 *
 * This modifies msc::win_list and msc::base, which requires msc::buf_mutex
 * to serialize, so the caller is expected to hold it.
 *
 * Return:	0 on success, -errno otherwise.
 */
static int msc_buffer_win_alloc(struct msc *msc, unsigned int nr_blocks)
{
	struct msc_window *win;
	unsigned long size = PAGE_SIZE;
	int i, ret = -ENOMEM;

	if (!nr_blocks)
		return 0;

	win = kzalloc(offsetof(struct msc_window, block[nr_blocks]),
		      GFP_KERNEL);
	if (!win)
		return -ENOMEM;

	if (!list_empty(&msc->win_list)) {
		struct msc_window *prev = list_entry(msc->win_list.prev,
						     struct msc_window, entry);

		win->pgoff = prev->pgoff + prev->nr_blocks;
	}

	for (i = 0; i < nr_blocks; i++) {
		win->block[i].bdesc =
			dma_alloc_coherent(msc_dev(msc)->parent->parent, size,
					   &win->block[i].addr, GFP_KERNEL);

		if (!win->block[i].bdesc)
			goto err_nomem;

#ifdef CONFIG_X86
		/* Set the page as uncached */
		set_memory_uc((unsigned long)win->block[i].bdesc, 1);
#endif
	}

	win->msc = msc;
	win->nr_blocks = nr_blocks;

	if (list_empty(&msc->win_list)) {
		msc->base = win->block[0].bdesc;
		msc->base_addr = win->block[0].addr;
	}

	list_add_tail(&win->entry, &msc->win_list);
	msc->nr_pages += nr_blocks;

	return 0;

err_nomem:
	for (i--; i >= 0; i--) {
#ifdef CONFIG_X86
		/* Reset the page to write-back before releasing */
		set_memory_wb((unsigned long)win->block[i].bdesc, 1);
#endif
		dma_free_coherent(msc_dev(msc), size, win->block[i].bdesc,
				  win->block[i].addr);
	}
	kfree(win);

	return ret;
}

/**
 * msc_buffer_win_free() - free a window from MSC's window list
 * @msc:	MSC device
 * @win:	window to free
 *
 * This modifies msc::win_list and msc::base, which requires msc::buf_mutex
 * to serialize, so the caller is expected to hold it.
 */
static void msc_buffer_win_free(struct msc *msc, struct msc_window *win)
{
	int i;

	msc->nr_pages -= win->nr_blocks;

	list_del(&win->entry);
	if (list_empty(&msc->win_list)) {
		msc->base = NULL;
		msc->base_addr = 0;
	}

	for (i = 0; i < win->nr_blocks; i++) {
		struct page *page = virt_to_page(win->block[i].bdesc);

		page->mapping = NULL;
#ifdef CONFIG_X86
		/* Reset the page to write-back before releasing */
		set_memory_wb((unsigned long)win->block[i].bdesc, 1);
#endif
		dma_free_coherent(msc_dev(win->msc), PAGE_SIZE,
				  win->block[i].bdesc, win->block[i].addr);
	}

	kfree(win);
}

/**
 * msc_buffer_relink() - set up block descriptors for multiblock mode
 * @msc:	MSC device
 *
 * This traverses msc::win_list, which requires msc::buf_mutex to serialize,
 * so the caller is expected to hold it.
 */
static void msc_buffer_relink(struct msc *msc)
{
	struct msc_window *win, *next_win;

	/* call with msc::mutex locked */
	list_for_each_entry(win, &msc->win_list, entry) {
		unsigned int blk;
		u32 sw_tag = 0;

		/*
		 * Last window's next_win should point to the first window
		 * and MSC_SW_TAG_LASTWIN should be set.
		 */
		if (msc_is_last_win(win)) {
			sw_tag |= MSC_SW_TAG_LASTWIN;
			next_win = list_entry(msc->win_list.next,
					      struct msc_window, entry);
		} else {
			next_win = list_entry(win->entry.next,
					      struct msc_window, entry);
		}

		for (blk = 0; blk < win->nr_blocks; blk++) {
			struct msc_block_desc *bdesc = win->block[blk].bdesc;

			memset(bdesc, 0, sizeof(*bdesc));

			bdesc->next_win = next_win->block[0].addr >> PAGE_SHIFT;

			/*
			 * Similarly to last window, last block should point
			 * to the first one.
			 */
			if (blk == win->nr_blocks - 1) {
				sw_tag |= MSC_SW_TAG_LASTBLK;
				bdesc->next_blk =
					win->block[0].addr >> PAGE_SHIFT;
			} else {
				bdesc->next_blk =
					win->block[blk + 1].addr >> PAGE_SHIFT;
			}

			bdesc->sw_tag = sw_tag;
			bdesc->block_sz = PAGE_SIZE / 64;
		}
	}

	/*
	 * Make the above writes globally visible before tracing is
	 * enabled to make sure hardware sees them coherently.
	 */
	wmb();
}

static void msc_buffer_multi_free(struct msc *msc)
{
	struct msc_window *win, *iter;

	list_for_each_entry_safe(win, iter, &msc->win_list, entry)
		msc_buffer_win_free(msc, win);
}

static int msc_buffer_multi_alloc(struct msc *msc, unsigned long *nr_pages,
				  unsigned int nr_wins)
{
	int ret, i;

	for (i = 0; i < nr_wins; i++) {
		ret = msc_buffer_win_alloc(msc, nr_pages[i]);
		if (ret) {
			msc_buffer_multi_free(msc);
			return ret;
		}
	}

	msc_buffer_relink(msc);

	return 0;
}

/**
 * msc_buffer_free() - free buffers for MSC
 * @msc:	MSC device
 *
 * Free MSC's storage buffers.
 *
 * This modifies msc::win_list and msc::base, which requires msc::buf_mutex to
 * serialize, so the caller is expected to hold it.
 */
static void msc_buffer_free(struct msc *msc)
{
	if (msc->mode == MSC_MODE_SINGLE)
		msc_buffer_contig_free(msc);
	else if (msc->mode == MSC_MODE_MULTI)
		msc_buffer_multi_free(msc);
}

/**
 * msc_buffer_alloc() - allocate a buffer for MSC
 * @msc:	MSC device
 * @size:	allocation size in bytes
 *
 * Allocate a storage buffer for MSC, depending on the msc::mode, it will be
 * either done via msc_buffer_contig_alloc() for SINGLE operation mode or
 * msc_buffer_win_alloc() for multiblock operation. The latter allocates one
 * window per invocation, so in multiblock mode this can be called multiple
 * times for the same MSC to allocate multiple windows.
 *
 * This modifies msc::win_list and msc::base, which requires msc::buf_mutex
 * to serialize, so the caller is expected to hold it.
 *
 * Return:	0 on success, -errno otherwise.
 */
static int msc_buffer_alloc(struct msc *msc, unsigned long *nr_pages,
			    unsigned int nr_wins)
{
	int ret;

	/* -1: buffer not allocated */
	if (atomic_read(&msc->user_count) != -1)
		return -EBUSY;

	if (msc->mode == MSC_MODE_SINGLE) {
		if (nr_wins != 1)
			return -EINVAL;

		ret = msc_buffer_contig_alloc(msc, nr_pages[0] << PAGE_SHIFT);
	} else if (msc->mode == MSC_MODE_MULTI) {
		ret = msc_buffer_multi_alloc(msc, nr_pages, nr_wins);
	} else {
		ret = -ENOTSUPP;
	}

	if (!ret) {
		/* allocation should be visible before the counter goes to 0 */
		smp_mb__before_atomic();

		if (WARN_ON_ONCE(atomic_cmpxchg(&msc->user_count, -1, 0) != -1))
			return -EINVAL;
	}

	return ret;
}

/**
 * msc_buffer_unlocked_free_unless_used() - free a buffer unless it's in use
 * @msc:	MSC device
 *
 * This will free MSC buffer unless it is in use or there is no allocated
 * buffer.
 * Caller needs to hold msc::buf_mutex.
 *
 * Return:	0 on successful deallocation or if there was no buffer to
 *		deallocate, -EBUSY if there are active users.
 */
static int msc_buffer_unlocked_free_unless_used(struct msc *msc)
{
	int count, ret = 0;

	count = atomic_cmpxchg(&msc->user_count, 0, -1);

	/* > 0: buffer is allocated and has users */
	if (count > 0)
		ret = -EBUSY;
	/* 0: buffer is allocated, no users */
	else if (!count)
		msc_buffer_free(msc);
	/* < 0: no buffer, nothing to do */

	return ret;
}

/**
 * msc_buffer_free_unless_used() - free a buffer unless it's in use
 * @msc:	MSC device
 *
 * This is a locked version of msc_buffer_unlocked_free_unless_used().
 */
static int msc_buffer_free_unless_used(struct msc *msc)
{
	int ret;

	mutex_lock(&msc->buf_mutex);
	ret = msc_buffer_unlocked_free_unless_used(msc);
	mutex_unlock(&msc->buf_mutex);

	return ret;
}

/**
 * msc_buffer_get_page() - get MSC buffer page at a given offset
 * @msc:	MSC device
 * @pgoff:	page offset into the storage buffer
 *
 * This traverses msc::win_list, so holding msc::buf_mutex is expected from
 * the caller.
 *
 * Return:	page if @pgoff corresponds to a valid buffer page or NULL.
 */
static struct page *msc_buffer_get_page(struct msc *msc, unsigned long pgoff)
{
	struct msc_window *win;

	if (msc->mode == MSC_MODE_SINGLE)
		return msc_buffer_contig_get_page(msc, pgoff);

	list_for_each_entry(win, &msc->win_list, entry)
		if (pgoff >= win->pgoff && pgoff < win->pgoff + win->nr_blocks)
			goto found;

	return NULL;

found:
	pgoff -= win->pgoff;
	return virt_to_page(win->block[pgoff].bdesc);
}

/**
 * struct msc_win_to_user_struct - data for copy_to_user() callback
 * @buf:	userspace buffer to copy data to
 * @offset:	running offset
 */
struct msc_win_to_user_struct {
	char __user	*buf;
	unsigned long	offset;
};

/**
 * msc_win_to_user() - iterator for msc_buffer_iterate() to copy data to user
 * @data:	callback's private data
 * @src:	source buffer
 * @len:	amount of data to copy from the source buffer
 */
static unsigned long msc_win_to_user(void *data, void *src, size_t len)
{
	struct msc_win_to_user_struct *u = data;
	unsigned long ret;

	ret = copy_to_user(u->buf + u->offset, src, len);
	u->offset += len - ret;

	return ret;
}


/*
 * file operations' callbacks
 */

static int intel_th_msc_open(struct inode *inode, struct file *file)
{
	struct intel_th_device *thdev = file->private_data;
	struct msc *msc = dev_get_drvdata(&thdev->dev);
	struct msc_iter *iter;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	iter = msc_iter_install(msc);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	file->private_data = iter;

	return nonseekable_open(inode, file);
}

static int intel_th_msc_release(struct inode *inode, struct file *file)
{
	struct msc_iter *iter = file->private_data;
	struct msc *msc = iter->msc;

	msc_iter_remove(iter, msc);

	return 0;
}

static ssize_t
msc_single_to_user(void *in_buf, unsigned long in_pages,
		   unsigned long in_sz, bool wrapped,
		   char __user *buf, loff_t off, size_t len)
{
	unsigned long size = in_pages << PAGE_SHIFT, rem = len;
	unsigned long start = off, tocopy = 0;

	/* With wrapping, copy the end of the buffer first */
	if (wrapped) {
		start += in_sz;
		if (start < size) {
			tocopy = min(rem, size - start);
			if (copy_to_user(buf, in_buf + start, tocopy))
				return -EFAULT;

			buf += tocopy;
			rem -= tocopy;
			start += tocopy;
		}

		start &= size - 1;
	}
	/* Copy the beginning of the buffer */
	if (rem) {
		tocopy = min(rem, in_sz - start);
		if (copy_to_user(buf, in_buf + start, tocopy))
			return -EFAULT;

		rem -= tocopy;
	}

	return len - rem;
}

static ssize_t intel_th_msc_read(struct file *file, char __user *buf,
				 size_t len, loff_t *ppos)
{
	struct msc_iter *iter = file->private_data;
	struct msc *msc = iter->msc;
	size_t size;
	loff_t off = *ppos;
	ssize_t ret = 0;

	if (!atomic_inc_unless_negative(&msc->user_count))
		return 0;

	if (msc->mode == MSC_MODE_SINGLE && !msc->single_wrap)
		size = msc->single_sz;
	else
		size = msc->nr_pages << PAGE_SHIFT;

	if (!size)
		goto put_count;

	if (off >= size)
		goto put_count;

	if (off + len >= size)
		len = size - off;

	if (msc->mode == MSC_MODE_SINGLE) {
		ret = msc_single_to_user(msc->base, msc->nr_pages,
					 msc->single_sz, msc->single_wrap,
					 buf, off, len);
		if (ret > 0)
			*ppos += ret;
	} else if (msc->mode == MSC_MODE_MULTI) {
		struct msc_win_to_user_struct u = {
			.buf	= buf,
			.offset	= 0,
		};

		ret = msc_buffer_iterate(iter, len, &u, msc_win_to_user);
		if (ret >= 0)
			*ppos = iter->offset;
	} else {
		ret = -ENOTSUPP;
	}

put_count:
	atomic_dec(&msc->user_count);

	return ret;
}

/*
 * vm operations callbacks (vm_ops)
 */

static void msc_mmap_open(struct vm_area_struct *vma)
{
	struct msc_iter *iter = vma->vm_file->private_data;
	struct msc *msc = iter->msc;

	atomic_inc(&msc->mmap_count);
}

static void msc_mmap_close(struct vm_area_struct *vma)
{
	struct msc_iter *iter = vma->vm_file->private_data;
	struct msc *msc = iter->msc;
	unsigned long pg;

	if (!atomic_dec_and_mutex_lock(&msc->mmap_count, &msc->buf_mutex))
		return;

	/* drop page _refcounts */
	for (pg = 0; pg < msc->nr_pages; pg++) {
		struct page *page = msc_buffer_get_page(msc, pg);

		if (WARN_ON_ONCE(!page))
			continue;

		if (page->mapping)
			page->mapping = NULL;
	}

	/* last mapping -- drop user_count */
	atomic_dec(&msc->user_count);
	mutex_unlock(&msc->buf_mutex);
}

static int msc_mmap_fault(struct vm_fault *vmf)
{
	struct msc_iter *iter = vmf->vma->vm_file->private_data;
	struct msc *msc = iter->msc;

	vmf->page = msc_buffer_get_page(msc, vmf->pgoff);
	if (!vmf->page)
		return VM_FAULT_SIGBUS;

	get_page(vmf->page);
	vmf->page->mapping = vmf->vma->vm_file->f_mapping;
	vmf->page->index = vmf->pgoff;

	return 0;
}

static const struct vm_operations_struct msc_mmap_ops = {
	.open	= msc_mmap_open,
	.close	= msc_mmap_close,
	.fault	= msc_mmap_fault,
};

static int intel_th_msc_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	struct msc_iter *iter = vma->vm_file->private_data;
	struct msc *msc = iter->msc;
	int ret = -EINVAL;

	if (!size || offset_in_page(size))
		return -EINVAL;

	if (vma->vm_pgoff)
		return -EINVAL;

	/* grab user_count once per mmap; drop in msc_mmap_close() */
	if (!atomic_inc_unless_negative(&msc->user_count))
		return -EINVAL;

	if (msc->mode != MSC_MODE_SINGLE &&
	    msc->mode != MSC_MODE_MULTI)
		goto out;

	if (size >> PAGE_SHIFT != msc->nr_pages)
		goto out;

	atomic_set(&msc->mmap_count, 1);
	ret = 0;

out:
	if (ret)
		atomic_dec(&msc->user_count);

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTCOPY;
	vma->vm_ops = &msc_mmap_ops;
	return ret;
}

static const struct file_operations intel_th_msc_fops = {
	.open		= intel_th_msc_open,
	.release	= intel_th_msc_release,
	.read		= intel_th_msc_read,
	.mmap		= intel_th_msc_mmap,
	.llseek		= no_llseek,
	.owner		= THIS_MODULE,
};

static void msc_wait_ple(struct intel_th_device *thdev)
{
	struct msc *msc = dev_get_drvdata(&thdev->dev);
	unsigned long count;
	u32 reg;

	for (reg = 0, count = MSC_PLE_WAITLOOP_DEPTH;
	     count && !(reg & MSCSTS_PLE); count--) {
		reg = ioread32(msc->reg_base + REG_MSU_MSC0STS);
		cpu_relax();
	}

	if (!count)
		dev_dbg(msc_dev(msc), "timeout waiting for MSC0 PLE\n");
}

#ifdef CONFIG_ACPI
#define ACPI_SIG_NPKT "NPKT"

/* Buffers that may be handed through NPKT ACPI table */
enum NPKT_BUF_TYPE {
	NPKT_MTB = 0,
	NPKT_MTB_REC,
	NPKT_CSR,
	NPKT_CSR_REC,
	NPKT_NBUF
};
static const char * const npkt_buf_name[NPKT_NBUF] = {
	[NPKT_MTB]	= "mtb",
	[NPKT_MTB_REC]	= "mtb_rec",
	[NPKT_CSR]	= "csr",
	[NPKT_CSR_REC]	= "csr_rec"
};

/* CSR capture still active */
#define NPKT_CSR_USED BIT(4)

struct acpi_npkt_buf {
	u64 addr;
	u32 size;
	u32 offset;
};

/* NPKT ACPI table */
struct acpi_table_npkt {
	struct acpi_table_header	header;
	struct acpi_npkt_buf		buffers[NPKT_NBUF];
	u8				flags;
} __packed;

/* Trace buffer obtained from NPKT table */
struct npkt_buf {
	dma_addr_t	phy;
	void		*buf;
	u32		size;
	u32		offset;
	bool		wrapped;
	atomic_t	active;
	struct msc	*msc;
};

static struct npkt_buf *npkt_bufs;
static struct dentry *npkt_dump_dir;
static DEFINE_MUTEX(npkt_lock);

/**
 * Stop current trace if a buffer was marked with a capture in pogress.
 *
 * Update buffer write offset and wrap status after stopping the trace.
 */
static void stop_buffer_trace(struct npkt_buf *buf)
{
	u32 reg, mode;
	struct msc *msc = buf->msc;

	mutex_lock(&npkt_lock);
	if (!atomic_read(&buf->active))
		goto unlock;

	reg = ioread32(msc->reg_base + REG_MSU_MSC0CTL);
	mode = (reg & MSC_MODE) >> __ffs(MSC_MODE);
	if (!(reg & MSC_EN) || mode != MSC_MODE_SINGLE) {
		/* Assume full buffer */
		pr_warn("NPKT reported CSR in use but not tracing to CSR\n");
		buf->offset = 0;
		buf->wrapped = true;
		atomic_set(&buf->active, 0);
		goto unlock;
	}

	/* The hub must be able to stop a capture not started by the driver */
	intel_th_trace_disable(msc->thdev);

	/* Update offset and wrap status */
	reg = ioread32(msc->reg_base + REG_MSU_MSC0MWP);
	buf->offset = reg - (u32)buf->phy;
	reg = ioread32(msc->reg_base + REG_MSU_MSC0STS);
	buf->wrapped = !!(reg & MSCSTS_WRAPSTAT);
	atomic_set(&buf->active, 0);

unlock:
	mutex_unlock(&npkt_lock);
}

/**
 * Copy re-ordered data from an NPKT buffer to a user buffer.
 */
static ssize_t read_npkt_dump_buf(struct file *file, char __user *user_buf,
				  size_t count, loff_t *ppos)
{
	struct npkt_buf *buf = file->private_data;
	size_t size = buf->size;
	loff_t off = *ppos;
	ssize_t ret;

	if (atomic_read(&buf->active))
		stop_buffer_trace(buf);

	if (off >= size)
		return 0;

	ret = msc_single_to_user(buf->buf, size >> PAGE_SHIFT,
				 buf->offset, buf->wrapped,
				 user_buf, off, count);
	if (ret > 0)
		*ppos += ret;

	return ret;
}

static const struct file_operations npkt_dump_buf_fops = {
	.read	= read_npkt_dump_buf,
	.open	= simple_open,
	.llseek	= noop_llseek,
};

/**
 * Prepare a buffer with remapped address for a given NPKT buffer and add
 * an entry for it in debugfs.
 */
static void npkt_bind_buffer(enum NPKT_BUF_TYPE type,
			     struct acpi_npkt_buf *abuf, u8 flags,
			     struct npkt_buf *buf, struct msc *msc)
{
	const char *name = npkt_buf_name[type];

	/* No buffer handed through ACPI */
	if (!abuf->addr || !abuf->size)
		return;

	/* Only expect multiples of page size */
	if (abuf->size & (PAGE_SIZE - 1)) {
		pr_warn("invalid size 0x%x for buffer %s\n",
			abuf->size, name);
		return;
	}

	buf->size = abuf->size;
	buf->offset = abuf->offset;
	buf->wrapped = !!(flags & BIT(type));
	/* CSR may still be active */
	if (type == NPKT_CSR && (flags & NPKT_CSR_USED)) {
		atomic_set(&buf->active, 1);
		buf->msc = msc;
	}

	buf->phy = abuf->addr;
	buf->buf = (__force void *)ioremap(buf->phy, buf->size);
	if (!buf->buf) {
		pr_err("ioremap failed for buffer %s 0x%llx size:0x%x\n",
		       name, buf->phy, buf->size);
		return;
	}

	debugfs_create_file(name, S_IRUGO, npkt_dump_dir, buf,
			    &npkt_dump_buf_fops);
}

static void npkt_bind_buffers(struct acpi_table_npkt *npkt,
			      struct npkt_buf *bufs, struct msc *msc)
{
	int i;

	for (i = 0; i < NPKT_NBUF; i++)
		npkt_bind_buffer(i, &npkt->buffers[i], npkt->flags,
				 &bufs[i], msc);
}

static void npkt_unbind_buffers(struct npkt_buf *bufs)
{
	int i;

	for (i = 0; i < NPKT_NBUF; i++)
		if (bufs[i].buf)
			iounmap((__force void __iomem *)bufs[i].buf);
}

/**
 * Prepare debugfs access to NPKT buffers.
 */
static void intel_th_npkt_init(struct msc *msc)
{
	acpi_status status;
	struct acpi_table_npkt *npkt;

	/* Associate NPKT to msc0 */
	if (npkt_bufs || msc->index != 0)
		return;

	status = acpi_get_table(ACPI_SIG_NPKT, 0,
				(struct acpi_table_header **)&npkt);
	if (ACPI_FAILURE(status)) {
		pr_warn("Failed to get NPKT table, %s\n",
			acpi_format_exception(status));
		return;
	}

	npkt_bufs = kzalloc(sizeof(struct npkt_buf) * NPKT_NBUF, GFP_KERNEL);
	if (!npkt_bufs)
		return;

	npkt_dump_dir = debugfs_create_dir("npkt_dump", NULL);
	if (!npkt_dump_dir) {
		pr_err("npkt_dump debugfs create dir failed\n");
		goto free_npkt_bufs;
	}

	npkt_bind_buffers(npkt, npkt_bufs, msc);

	return;

free_npkt_bufs:
	kfree(npkt_bufs);
	npkt_bufs = NULL;
}

/**
 * Remove debugfs access to NPKT buffers and release resources.
 */
static void intel_th_npkt_remove(struct msc *msc)
{
	/* Only clean for msc 0 if necessary */
	if (!npkt_bufs || msc->index != 0)
		return;

	npkt_unbind_buffers(npkt_bufs);
	debugfs_remove_recursive(npkt_dump_dir);
	kfree(npkt_bufs);
	npkt_bufs = NULL;
}

/**
 * First trace callback.
 *
 * If NPKT notified a CSR capture is in progress, stop it and update buffer
 * write offset and wrap status.
 */
static void intel_th_msc_first_trace(struct intel_th_device *thdev)
{
	struct device *dev = &thdev->dev;
	struct msc *msc = dev_get_drvdata(dev);
	struct npkt_buf *buf;

	if (!npkt_bufs || msc->index != 0)
		return;

	buf = &npkt_bufs[NPKT_CSR];
	if (atomic_read(&buf->active))
		stop_buffer_trace(buf);
}

#else /* !CONFIG_ACPI */
static inline void intel_th_npkt_init(struct msc *msc) {}
static inline void intel_th_npkt_remove(struct msc *msc) {}
#define intel_th_msc_first_trace NULL
#endif /* !CONFIG_ACPI */

static int intel_th_msc_init(struct msc *msc)
{
	atomic_set(&msc->user_count, -1);

	msc->mode = MSC_MODE_MULTI;
	mutex_init(&msc->buf_mutex);
	INIT_LIST_HEAD(&msc->win_list);
	INIT_LIST_HEAD(&msc->iter_list);

	msc->burst_len =
		(ioread32(msc->reg_base + REG_MSU_MSC0CTL) & MSC_LEN) >>
		__ffs(MSC_LEN);

	msc->thdev->output.wait_empty = msc_wait_ple;

	return 0;
}

static const char * const msc_mode[] = {
	[MSC_MODE_SINGLE]	= "single",
	[MSC_MODE_MULTI]	= "multi",
	[MSC_MODE_EXI]		= "ExI",
	[MSC_MODE_DEBUG]	= "debug",
};

static ssize_t
wrap_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct msc *msc = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", msc->wrap);
}

static ssize_t
wrap_store(struct device *dev, struct device_attribute *attr, const char *buf,
	   size_t size)
{
	struct msc *msc = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	msc->wrap = !!val;

	return size;
}

static DEVICE_ATTR_RW(wrap);

static ssize_t
mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct msc *msc = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%s\n", msc_mode[msc->mode]);
}

static ssize_t
mode_store(struct device *dev, struct device_attribute *attr, const char *buf,
	   size_t size)
{
	struct msc *msc = dev_get_drvdata(dev);
	size_t len = size;
	char *cp;
	int i, ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	cp = memchr(buf, '\n', len);
	if (cp)
		len = cp - buf;

	for (i = 0; i < ARRAY_SIZE(msc_mode); i++)
		if (!strncmp(msc_mode[i], buf, len))
			goto found;

	return -EINVAL;

found:
	mutex_lock(&msc->buf_mutex);
	ret = msc_buffer_unlocked_free_unless_used(msc);
	if (!ret)
		msc->mode = i;
	mutex_unlock(&msc->buf_mutex);

	return ret ? ret : size;
}

static DEVICE_ATTR_RW(mode);

static ssize_t
nr_pages_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct msc *msc = dev_get_drvdata(dev);
	struct msc_window *win;
	size_t count = 0;

	mutex_lock(&msc->buf_mutex);

	if (msc->mode == MSC_MODE_SINGLE)
		count = scnprintf(buf, PAGE_SIZE, "%ld\n", msc->nr_pages);
	else if (msc->mode == MSC_MODE_MULTI) {
		list_for_each_entry(win, &msc->win_list, entry) {
			count += scnprintf(buf + count, PAGE_SIZE - count,
					   "%d%c", win->nr_blocks,
					   msc_is_last_win(win) ? '\n' : ',');
		}
	} else {
		count = scnprintf(buf, PAGE_SIZE, "unsupported\n");
	}

	mutex_unlock(&msc->buf_mutex);

	return count;
}

static ssize_t
nr_pages_store(struct device *dev, struct device_attribute *attr,
	       const char *buf, size_t size)
{
	struct msc *msc = dev_get_drvdata(dev);
	unsigned long val, *win = NULL, *rewin;
	size_t len = size;
	const char *p = buf;
	char *end, *s;
	int ret, nr_wins = 0;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	ret = msc_buffer_free_unless_used(msc);
	if (ret)
		return ret;

	msc->max_blocks = 0;

	/* scan the comma-separated list of allocation sizes */
	end = memchr(buf, '\n', len);
	if (end)
		len = end - buf;

	do {
		end = memchr(p, ',', len);
		s = kstrndup(p, end ? end - p : len, GFP_KERNEL);
		if (!s) {
			ret = -ENOMEM;
			goto free_win;
		}

		ret = kstrtoul(s, 10, &val);
		kfree(s);

		if (ret || !val)
			goto free_win;

		if (nr_wins && msc->mode == MSC_MODE_SINGLE) {
			ret = -EINVAL;
			goto free_win;
		}

		nr_wins++;
		rewin = krealloc(win, sizeof(*win) * nr_wins, GFP_KERNEL);
		if (!rewin) {
			kfree(win);
			return -ENOMEM;
		}

		win = rewin;
		win[nr_wins - 1] = val;

		msc->max_blocks =
			(val > msc->max_blocks) ? val : msc->max_blocks;

		if (!end)
			break;

		len -= end - p;
		p = end + 1;
	} while (len);

	mutex_lock(&msc->buf_mutex);
	ret = msc_buffer_alloc(msc, win, nr_wins);
	mutex_unlock(&msc->buf_mutex);

free_win:
	kfree(win);

	return ret ? ret : size;
}

static DEVICE_ATTR_RW(nr_pages);

static ssize_t
win_switch_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t size)
{
	struct msc *msc = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	if (val != 1)
		return -EINVAL;

	intel_th_trace_switch(msc->thdev);
	return size;
}

static DEVICE_ATTR_WO(win_switch);

static struct attribute *msc_output_attrs[] = {
	&dev_attr_wrap.attr,
	&dev_attr_mode.attr,
	&dev_attr_nr_pages.attr,
	&dev_attr_win_switch.attr,
	NULL,
};

static struct attribute_group msc_output_group = {
	.attrs	= msc_output_attrs,
};

static int intel_th_msc_probe(struct intel_th_device *thdev)
{
	struct device *dev = &thdev->dev;
	struct resource *res;
	struct msc *msc;
	void __iomem *base;
	int err;

	res = intel_th_device_get_resource(thdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	base = devm_ioremap(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	msc = devm_kzalloc(dev, sizeof(*msc), GFP_KERNEL);
	if (!msc)
		return -ENOMEM;

	msc->index = thdev->id;

	msc->thdev = thdev;
	msc->reg_base = base + msc->index * 0x100;

	err = intel_th_msc_init(msc);
	if (err)
		return err;

	msc->max_blocks = 0;
	dev_set_drvdata(dev, msc);

	intel_th_npkt_init(msc);
	msc_add_instance(thdev);

	return 0;
}

static void intel_th_msc_remove(struct intel_th_device *thdev)
{
	struct msc *msc = dev_get_drvdata(&thdev->dev);
	intel_th_npkt_remove(msc);
	msc_rm_instance(thdev);
	sysfs_remove_group(&thdev->dev.kobj, &msc_output_group);
}

static struct intel_th_driver intel_th_msc_driver = {
	.first_trace	= intel_th_msc_first_trace,
	.probe	= intel_th_msc_probe,
	.remove	= intel_th_msc_remove,
	.activate	= intel_th_msc_activate,
	.deactivate	= intel_th_msc_deactivate,
	.fops	= &intel_th_msc_fops,
	.attr_group	= &msc_output_group,
	.driver	= {
		.name	= "msc",
		.owner	= THIS_MODULE,
	},
};

module_driver(intel_th_msc_driver,
	      intel_th_driver_register,
	      intel_th_driver_unregister);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel(R) Trace Hub Memory Storage Unit driver");
MODULE_AUTHOR("Alexander Shishkin <alexander.shishkin@linux.intel.com>");
