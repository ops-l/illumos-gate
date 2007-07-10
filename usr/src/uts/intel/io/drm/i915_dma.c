/* BEGIN CSTYLED */

/* i915_dma.c -- DMA support for the I915 -*- linux-c -*-
 */
/*
 * Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include "drmP.h"
#include "drm.h"
#include "i915_drm.h"
#include "i915_drv.h"

#define IS_I965G(dev)  (dev->pci_device == 0x2972 || \
			dev->pci_device == 0x2982 || \
			dev->pci_device == 0x2992 || \
			dev->pci_device == 0x29A2 || \
    			dev->pci_device == 0x2A02 || \
    			dev->pci_device == 0x2A12)


/* Really want an OS-independent resettable timer.  Would like to have
 * this loop run for (eg) 3 sec, but have the timer reset every time
 * the head pointer changes, so that EBUSY only happens if the ring
 * actually stalls for (eg) 3 seconds.
 */
/*ARGSUSED*/
int i915_wait_ring(drm_device_t * dev, int n, const char *caller)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	drm_i915_ring_buffer_t *ring = &(dev_priv->ring);
	u32 last_head = I915_READ(LP_RING + RING_HEAD) & HEAD_ADDR;
	int i;

	for (i = 0; i < 10000; i++) {
		ring->head = I915_READ(LP_RING + RING_HEAD) & HEAD_ADDR;
		ring->space = ring->head - (ring->tail + 8);
		if (ring->space < 0)
			ring->space += ring->Size;
		if (ring->space >= n)
			return 0;

		dev_priv->sarea_priv->perf_boxes |= I915_BOX_WAIT;

		if (ring->head != last_head)
			i = 0;

		last_head = ring->head;
		DRM_UDELAY(1);
	}

	return DRM_ERR(EBUSY);
}

void i915_kernel_lost_context(drm_device_t * dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	drm_i915_ring_buffer_t *ring = &(dev_priv->ring);

	ring->head = I915_READ(LP_RING + RING_HEAD) & HEAD_ADDR;
	ring->tail = I915_READ(LP_RING + RING_TAIL) & TAIL_ADDR;
	ring->space = ring->head - (ring->tail + 8);
	if (ring->space < 0)
		ring->space += ring->Size;

	if (ring->head == ring->tail)
		dev_priv->sarea_priv->perf_boxes |= I915_BOX_RING_EMPTY;
}

static int i915_dma_cleanup(drm_device_t * dev)
{
	/* Make sure interrupts are disabled here because the uninstall ioctl
	 * may not have been called from userspace and after dev_private
	 * is freed, it's too late.
	 */
	if (dev->irq)
		(void) drm_irq_uninstall(dev);

	if (dev->dev_private) {
		drm_i915_private_t *dev_priv =
		    (drm_i915_private_t *) dev->dev_private;

		if (dev_priv->ring.virtual_start) {
			drm_core_ioremapfree(&dev_priv->ring.map, dev);
		}

#if defined(__SOLARIS__) || defined(sun)
		if (dev_priv->hw_status_page) {
			drm_pci_free(dev);
#else
		if (dev_priv->status_page_dmah) {
			drm_pci_free(dev, dev_priv->status_page_dmah);
#endif
			/* Need to rewrite hardware status page */
			I915_WRITE(0x02080, 0x1ffff000);
		}

		drm_free(dev->dev_private, sizeof(drm_i915_private_t),
			 DRM_MEM_DRIVER);

		dev->dev_private = NULL;
	}

	return 0;
}

static int i915_initialize(drm_device_t * dev,
			   drm_i915_private_t * dev_priv,
			   drm_i915_init_t * init)
{
	(void) memset(dev_priv, 0, sizeof(drm_i915_private_t));

	DRM_GETSAREA();
	if (!dev_priv->sarea) {
		DRM_ERROR("can not find sarea!\n");
		dev->dev_private = (void *)dev_priv;
		(void) i915_dma_cleanup(dev);
		return DRM_ERR(EINVAL);
	}

	dev_priv->mmio_map = drm_core_findmap(dev, init->mmio_offset);
	if (!dev_priv->mmio_map) {
		dev->dev_private = (void *)dev_priv;
		(void) i915_dma_cleanup(dev);
		DRM_ERROR("can not find mmio map!\n");
		return DRM_ERR(EINVAL);
	}

	dev_priv->sarea_priv = (drm_i915_sarea_t *)
	    ((u8 *) dev_priv->sarea->handle + init->sarea_priv_offset);

	dev_priv->ring.Start = init->ring_start;
	dev_priv->ring.End = init->ring_end;
	dev_priv->ring.Size = init->ring_size;
	dev_priv->ring.tail_mask = dev_priv->ring.Size - 1;

	dev_priv->ring.map.offset.off = (u_offset_t)init->ring_start;
	dev_priv->ring.map.size = init->ring_size;
	dev_priv->ring.map.type = 0;
	dev_priv->ring.map.flags = 0;
	dev_priv->ring.map.mtrr = 0;

	drm_core_ioremap(&dev_priv->ring.map, dev);

	if (dev_priv->ring.map.handle == NULL) {
		dev->dev_private = (void *)dev_priv;
		(void) i915_dma_cleanup(dev);
		DRM_ERROR("can not ioremap virtual address for"
			  " ring buffer\n");
		return DRM_ERR(ENOMEM);
	}

	dev_priv->ring.virtual_start = (u8 *)dev_priv->ring.map.dev_addr;

	dev_priv->cpp = init->cpp;
	dev_priv->back_offset = init->back_offset;
	dev_priv->front_offset = init->front_offset;
	dev_priv->current_page = 0;
	dev_priv->sarea_priv->pf_current_page = dev_priv->current_page;

	/* We are using separate values as placeholders for mechanisms for
	 * private backbuffer/depthbuffer usage.
	 */
	dev_priv->use_mi_batchbuffer_start = 0;

	/* Allow hardware batchbuffers unless told otherwise.
	 */
	dev_priv->allow_batchbuffer = 1;

	/* Program Hardware Status Page */
#if defined(__SOLARIS__) || defined(sun)
	dev_priv->hw_status_page =
	    drm_pci_alloc(dev, DRM_PAGE_SIZE, &dev_priv->dma_status_page);
#else
	dev_priv->status_page_dmah = drm_pci_alloc(dev, PAGE_SIZE, PAGE_SIZE,
	    0xffffffff);
#endif

#if defined(__SOLARIS__) || defined(sun)
	if (!dev_priv->hw_status_page) {
#else
	if (!dev_priv->status_page_dmah) {
#endif
		dev->dev_private = (void *)dev_priv;
		(void) i915_dma_cleanup(dev);
		DRM_ERROR("Can not allocate hardware status page\n");
		return DRM_ERR(ENOMEM);
	}

#if !defined(__SOLARIS__) && !defined(sun)
	dev_priv->hw_status_page = dev_priv->status_page_dmah->vaddr;
	dev_priv->dma_status_page = dev_priv->status_page_dmah->busaddr;
#endif
	(void) memset(dev_priv->hw_status_page, 0, PAGE_SIZE);
	DRM_DEBUG("hw status page @ %p\n", dev_priv->hw_status_page);

	I915_WRITE(0x02080, dev_priv->dma_status_page);
	DRM_DEBUG("Enabled hardware status page\n");

	dev->dev_private = (void *)dev_priv;

#ifdef I915_HAVE_BUFFER
	drm_bo_driver_init(dev);
#endif
	return 0;
}

static int i915_dma_resume(drm_device_t * dev)
{
	drm_i915_private_t *dev_priv = (drm_i915_private_t *) dev->dev_private;

	DRM_DEBUG("%s\n", __FUNCTION__);

	if (!dev_priv->sarea) {
		DRM_ERROR("can not find sarea!\n");
		return DRM_ERR(EINVAL);
	}

	if (!dev_priv->mmio_map) {
		DRM_ERROR("can not find mmio map!\n");
		return DRM_ERR(EINVAL);
	}

	if (dev_priv->ring.map.handle == NULL) {
		DRM_ERROR("can not ioremap virtual address for"
			  " ring buffer\n");
		return DRM_ERR(ENOMEM);
	}

	/* Program Hardware Status Page */
	if (!dev_priv->hw_status_page) {
		DRM_ERROR("Can not find hardware status page\n");
		return DRM_ERR(EINVAL);
	}
	DRM_DEBUG("hw status page @ %p\n", dev_priv->hw_status_page);

	I915_WRITE(0x02080, dev_priv->dma_status_page);
	DRM_DEBUG("Enabled hardware status page\n");

	return 0;
}

/*ARGSUSED*/
static int i915_dma_init(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_i915_private_t *dev_priv;
	drm_i915_init_t init;
	int retcode = 0;

	DRM_COPY_FROM_USER_IOCTL(init, (drm_i915_init_t __user *) data,
				 sizeof(init));

	switch (init.func) {
	case I915_INIT_DMA:
		dev_priv = drm_alloc(sizeof(drm_i915_private_t),
				     DRM_MEM_DRIVER);
		if (dev_priv == NULL)
			return DRM_ERR(ENOMEM);
		retcode = i915_initialize(dev, dev_priv, &init);
		break;
	case I915_CLEANUP_DMA:
		retcode = i915_dma_cleanup(dev);
		break;
	case I915_RESUME_DMA:
		retcode = i915_dma_resume(dev);
		break;
	default:
		retcode = DRM_ERR(EINVAL);
		break;
	}

	return retcode;
}

/* Implement basically the same security restrictions as hardware does
 * for MI_BATCH_NON_SECURE.  These can be made stricter at any time.
 *
 * Most of the calculations below involve calculating the size of a
 * particular instruction.  It's important to get the size right as
 * that tells us where the next instruction to check is.  Any illegal
 * instruction detected will be given a size of zero, which is a
 * signal to abort the rest of the buffer.
 */
static int do_validate_cmd(int cmd)
{
	switch (((cmd >> 29) & 0x7)) {
	case 0x0:
		switch ((cmd >> 23) & 0x3f) {
		case 0x0:
			return 1;	/* MI_NOOP */
		case 0x4:
			return 1;	/* MI_FLUSH */
		default:
			return 0;	/* disallow everything else */
		}
#ifndef __SUNPRO_C
		break;
#endif
	case 0x1:
		return 0;	/* reserved */
	case 0x2:
		return (cmd & 0xff) + 2;	/* 2d commands */
	case 0x3:
		if (((cmd >> 24) & 0x1f) <= 0x18)
			return 1;

		switch ((cmd >> 24) & 0x1f) {
		case 0x1c:
			return 1;
		case 0x1d:
			switch ((cmd >> 16) & 0xff) {
			case 0x3:
				return (cmd & 0x1f) + 2;
			case 0x4:
				return (cmd & 0xf) + 2;
			default:
				return (cmd & 0xffff) + 2;
			}
		case 0x1e:
			if (cmd & (1 << 23))
				return (cmd & 0xffff) + 1;
			else
				return 1;
		case 0x1f:
			if ((cmd & (1 << 23)) == 0)	/* inline vertices */
				return (cmd & 0x1ffff) + 2;
			else if (cmd & (1 << 17))	/* indirect random */
				if ((cmd & 0xffff) == 0)
					return 0;	/* unknown length, too hard */
				else
					return (((cmd & 0xffff) + 1) / 2) + 1;
			else
				return 2;	/* indirect sequential */
		default:
			return 0;
		}
	default:
		return 0;
	}

#ifndef __SUNPRO_C
	return 0;
#endif
}

static int validate_cmd(int cmd)
{
	int ret = do_validate_cmd(cmd);

/* 	printk("validate_cmd( %x ): %d\n", cmd, ret); */

	return ret;
}

static int i915_emit_cmds(drm_device_t * dev, int __user * buffer, int dwords, int mode)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	int i;
	RING_LOCALS;

	if ((dwords+1) * sizeof(int) >= dev_priv->ring.Size - 8)
		return DRM_ERR(EINVAL);

	BEGIN_LP_RING((dwords+1)&~1);

	for (i = 0; i < dwords;) {
		int cmd, sz;

		if (DRM_COPY_FROM_USER_UNCHECKED(&cmd, &buffer[i], sizeof(cmd)))
			return DRM_ERR(EINVAL);


		if ((sz = validate_cmd(cmd)) == 0 || i + sz > dwords)
			return DRM_ERR(EINVAL);

		OUT_RING(cmd);

		while (++i, --sz) {
			if (DRM_COPY_FROM_USER_UNCHECKED(&cmd, &buffer[i],
							 sizeof(cmd))) {
				return DRM_ERR(EINVAL);
			}
			OUT_RING(cmd);
		}
	}

	if (dwords & 1)
		OUT_RING(0);

	ADVANCE_LP_RING();
		
	return 0;
}

static int i915_emit_box(drm_device_t * dev,
			 drm_clip_rect_t __user * boxes,
			 int i, int DR1, int DR4, int mode)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	drm_clip_rect_t box;
	RING_LOCALS;

	if (DRM_COPY_FROM_USER_UNCHECKED(&box, &boxes[i], sizeof(box))) {
		return DRM_ERR(EFAULT);
	}

	if (box.y2 <= box.y1 || box.x2 <= box.x1 || box.y2 <= 0 || box.x2 <= 0) {
		DRM_ERROR("Bad box %d,%d..%d,%d\n",
			  box.x1, box.y1, box.x2, box.y2);
		return DRM_ERR(EINVAL);
	}

	if (IS_I965G(dev)) {
		BEGIN_LP_RING(4);
		OUT_RING(GFX_OP_DRAWRECT_INFO_I965);
		OUT_RING((box.x1 & 0xffff) | (box.y1 << 16));
		OUT_RING(((box.x2 - 1) & 0xffff) | ((box.y2 - 1) << 16));
		OUT_RING(DR4);
		ADVANCE_LP_RING();
	} else {
		BEGIN_LP_RING(6);
		OUT_RING(GFX_OP_DRAWRECT_INFO);
		OUT_RING(DR1);
		OUT_RING((box.x1 & 0xffff) | (box.y1 << 16));
		OUT_RING(((box.x2 - 1) & 0xffff) | ((box.y2 - 1) << 16));
		OUT_RING(DR4);
		OUT_RING(0);
		ADVANCE_LP_RING();
	}

	return 0;
}

/* XXX: Emitting the counter should really be moved to part of the IRQ
 * emit.  For now, do it in both places:
 */

static void i915_emit_breadcrumb(drm_device_t *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	RING_LOCALS;

	dev_priv->sarea_priv->last_enqueue = ++dev_priv->counter;

	BEGIN_LP_RING(4);
	OUT_RING(CMD_STORE_DWORD_IDX);
	OUT_RING(20);
	OUT_RING(dev_priv->counter);
	OUT_RING(0);
	ADVANCE_LP_RING();
#ifdef I915_HAVE_FENCE
	drm_fence_flush_old(dev, 0, dev_priv->counter);
#endif
}


int i915_emit_mi_flush(drm_device_t *dev, uint32_t flush)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	uint32_t flush_cmd = CMD_MI_FLUSH;
	RING_LOCALS;

	flush_cmd |= flush;

	i915_kernel_lost_context(dev);

	BEGIN_LP_RING(4);
	OUT_RING(flush_cmd);
	OUT_RING(0);
	OUT_RING(0);
	OUT_RING(0);
	ADVANCE_LP_RING();

	return 0;
}

static int i915_dispatch_cmdbuffer(drm_device_t * dev,
				   drm_i915_cmdbuffer_t * cmd, int mode)
{
	int nbox = cmd->num_cliprects;
	int i = 0, count, ret;

	if (cmd->sz & 0x3) {
		DRM_ERROR("alignment");
		return DRM_ERR(EINVAL);
	}

	i915_kernel_lost_context(dev);

	count = nbox ? nbox : 1;

	for (i = 0; i < count; i++) {
		if (i < nbox) {
			ret = i915_emit_box(dev, cmd->cliprects, i,
					    cmd->DR1, cmd->DR4, mode);
			if (ret)
				return ret;
		}

		ret = i915_emit_cmds(dev, (int __user *)cmd->buf, cmd->sz / 4, mode);
		if (ret)
			return ret;
	}

	i915_emit_breadcrumb( dev );
	return 0;
}

static int i915_dispatch_batchbuffer(drm_device_t * dev,
				     drm_i915_batchbuffer_t * batch, int mode)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	drm_clip_rect_t __user *boxes = batch->cliprects;
	int nbox = batch->num_cliprects;
	int i = 0, count;
	RING_LOCALS;

	if ((batch->start | batch->used) & 0x7) {
		DRM_ERROR("alignment");
		return DRM_ERR(EINVAL);
	}

	i915_kernel_lost_context(dev);

	count = nbox ? nbox : 1;

	for (i = 0; i < count; i++) {
		if (i < nbox) {
			int ret = i915_emit_box(dev, boxes, i,
						batch->DR1, batch->DR4, mode);
			if (ret)
				return ret;
		}

		if (dev_priv->use_mi_batchbuffer_start) {
			BEGIN_LP_RING(2);
			OUT_RING(MI_BATCH_BUFFER_START | (2 << 6));
			OUT_RING(batch->start | MI_BATCH_NON_SECURE);
			ADVANCE_LP_RING();
		} else {
			BEGIN_LP_RING(4);
			OUT_RING(MI_BATCH_BUFFER);
			OUT_RING(batch->start | MI_BATCH_NON_SECURE);
			OUT_RING(batch->start + batch->used - 4);
			OUT_RING(0);
			ADVANCE_LP_RING();
		}
	}

	i915_emit_breadcrumb( dev );

	return 0;
}

static int i915_dispatch_flip(drm_device_t * dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	RING_LOCALS;

	DRM_DEBUG("%s: page=%d pfCurrentPage=%d\n",
		  __FUNCTION__,
		  dev_priv->current_page,
		  dev_priv->sarea_priv->pf_current_page);

	i915_kernel_lost_context(dev);

	BEGIN_LP_RING(2);
	OUT_RING(INST_PARSER_CLIENT | INST_OP_FLUSH | INST_FLUSH_MAP_CACHE);
	OUT_RING(0);
	ADVANCE_LP_RING();

	BEGIN_LP_RING(6);
	OUT_RING(CMD_OP_DISPLAYBUFFER_INFO | ASYNC_FLIP);
	OUT_RING(0);
	if (dev_priv->current_page == 0) {
		OUT_RING(dev_priv->back_offset);
		dev_priv->current_page = 1;
	} else {
		OUT_RING(dev_priv->front_offset);
		dev_priv->current_page = 0;
	}
	OUT_RING(0);
	ADVANCE_LP_RING();

	BEGIN_LP_RING(2);
	OUT_RING(MI_WAIT_FOR_EVENT | MI_WAIT_FOR_PLANE_A_FLIP);
	OUT_RING(0);
	ADVANCE_LP_RING();

	dev_priv->sarea_priv->last_enqueue = dev_priv->counter++;

	BEGIN_LP_RING(4);
	OUT_RING(CMD_STORE_DWORD_IDX);
	OUT_RING(20);
	OUT_RING(dev_priv->counter);
	OUT_RING(0);
	ADVANCE_LP_RING();
#ifdef I915_HAVE_FENCE
	drm_fence_flush_old(dev, 0, dev_priv->counter);
#endif
	dev_priv->sarea_priv->pf_current_page = dev_priv->current_page;
	return 0;
}

static int i915_quiescent(drm_device_t * dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;

	i915_kernel_lost_context(dev);
	return i915_wait_ring(dev, dev_priv->ring.Size - 8, __FUNCTION__);
}

/*ARGSUSED*/
static int i915_flush_ioctl(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;

	LOCK_TEST_WITH_RETURN(dev, filp);

	return i915_quiescent(dev);
}

/*ARGSUSED*/
static int i915_batchbuffer(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_i915_private_t *dev_priv = (drm_i915_private_t *) dev->dev_private;
	u32 *hw_status = dev_priv->hw_status_page;
	drm_i915_sarea_t *sarea_priv = (drm_i915_sarea_t *)
	    dev_priv->sarea_priv;
	drm_i915_batchbuffer_t batch;
	int ret;

	if (!dev_priv->allow_batchbuffer) {
		DRM_ERROR("Batchbuffer ioctl disabled\n");
		return DRM_ERR(EINVAL);
	}

	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		drm_i915_batchbuffer32_t batchbuffer32_t;

		DRM_COPY_FROM_USER_IOCTL(batchbuffer32_t,
			(drm_i915_batchbuffer32_t __user *) data,
			sizeof (drm_i915_batchbuffer32_t));

		batch.start = batchbuffer32_t.start;
		batch.used = batchbuffer32_t.used;
		batch.DR1 = batchbuffer32_t.DR1;
		batch.DR4 = batchbuffer32_t.DR4;
		batch.num_cliprects = batchbuffer32_t.num_cliprects;
		batch.cliprects = (drm_clip_rect_t __user *)
			(uintptr_t)batchbuffer32_t.cliprects;
	} else
		DRM_COPY_FROM_USER_IOCTL(batch, (drm_i915_batchbuffer_t __user *) data,
			sizeof(batch));

	DRM_DEBUG("i915 batchbuffer, start %x used %d cliprects %d\n",
		  batch.start, batch.used, batch.num_cliprects);

	LOCK_TEST_WITH_RETURN(dev, filp);
	/*

	if (batch.num_cliprects && DRM_VERIFYAREA_READ(batch.cliprects,
						       batch.num_cliprects *
						       sizeof(drm_clip_rect_t)))
		return DRM_ERR(EFAULT);
		*/

	ret = i915_dispatch_batchbuffer(dev, &batch, mode);

	sarea_priv->last_dispatch = (int)hw_status[5];
	return ret;
}

/*ARGSUSED*/
static int i915_cmdbuffer(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_i915_private_t *dev_priv = (drm_i915_private_t *) dev->dev_private;
	u32 *hw_status = dev_priv->hw_status_page;
	drm_i915_sarea_t *sarea_priv = (drm_i915_sarea_t *)
	    dev_priv->sarea_priv;
	drm_i915_cmdbuffer_t cmdbuf;
	int ret;

	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		drm_i915_cmdbuffer32_t cmdbuffer32_t;

		DRM_COPY_FROM_USER_IOCTL(cmdbuffer32_t,
			(drm_i915_cmdbuffer32_t __user *) data,
			sizeof (drm_i915_cmdbuffer32_t));

		cmdbuf.buf = (char __user *)(uintptr_t)cmdbuffer32_t.buf;
		cmdbuf.sz = cmdbuffer32_t.sz;
		cmdbuf.DR1 = cmdbuffer32_t.DR1;
		cmdbuf.DR4 = cmdbuffer32_t.DR4;
		cmdbuf.num_cliprects = cmdbuffer32_t.num_cliprects;
		cmdbuf.cliprects = (drm_clip_rect_t __user *)
			(uintptr_t)cmdbuffer32_t.cliprects;
	} else
		DRM_COPY_FROM_USER_IOCTL(cmdbuf, (drm_i915_cmdbuffer_t __user *) data,
			sizeof(cmdbuf));

	DRM_DEBUG("i915 cmdbuffer, buf %p sz %d cliprects %d\n",
		  cmdbuf.buf, cmdbuf.sz, cmdbuf.num_cliprects);

	LOCK_TEST_WITH_RETURN(dev, filp);
	/*

	if (cmdbuf.num_cliprects &&
	    DRM_VERIFYAREA_READ(cmdbuf.cliprects,
				cmdbuf.num_cliprects *
				sizeof(drm_clip_rect_t))) {
		DRM_ERROR("Fault accessing cliprects\n");
		return DRM_ERR(EFAULT);
	}
	*/

	ret = i915_dispatch_cmdbuffer(dev, &cmdbuf, mode);
	if (ret) {
		DRM_ERROR("i915_dispatch_cmdbuffer failed\n");
		return ret;
	}

	sarea_priv->last_dispatch = (int)hw_status[5];
	return 0;
}

static int i915_do_cleanup_pageflip(drm_device_t * dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;

	DRM_DEBUG("%s\n", __FUNCTION__);
	if (dev_priv->current_page != 0)
		(void) i915_dispatch_flip(dev);

	return 0;
}

/*ARGSUSED*/
static int i915_flip_bufs(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;

	DRM_DEBUG("%s\n", __FUNCTION__);

	LOCK_TEST_WITH_RETURN(dev, filp);

	return i915_dispatch_flip(dev);
}

/*ARGSUSED*/
static int i915_getparam(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_i915_private_t *dev_priv = dev->dev_private;
	drm_i915_getparam_t param;
	int value;

	if (!dev_priv) {
		DRM_ERROR("%s called with no initialization\n", __FUNCTION__);
		return DRM_ERR(EINVAL);
	}

	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		drm_i915_getparam32_t getparam32_t;

		DRM_COPY_FROM_USER_IOCTL(getparam32_t,
			(drm_i915_getparam32_t __user *) data,
			sizeof (drm_i915_getparam32_t));

		param.param = getparam32_t.param;
		param.value = (int __user *)(uintptr_t)getparam32_t.value;
	} else
		DRM_COPY_FROM_USER_IOCTL(param, (drm_i915_getparam_t __user *) data,
			sizeof(param));

	switch (param.param) {
	case I915_PARAM_IRQ_ACTIVE:
		value = dev->irq ? 1 : 0;
		break;
	case I915_PARAM_ALLOW_BATCHBUFFER:
		value = dev_priv->allow_batchbuffer ? 1 : 0;
		break;
	case I915_PARAM_LAST_DISPATCH:
		value = READ_BREADCRUMB(dev_priv);
		break;
	default:
		DRM_ERROR("Unknown parameter %d\n", param.param);
		return DRM_ERR(EINVAL);
	}

	if (DRM_COPY_TO_USER(param.value, &value, sizeof(int))) {
		DRM_ERROR("i915_getparam failed\n");
		return DRM_ERR(EFAULT);
	}
	return 0;
}

/*ARGSUSED*/
static int i915_setparam(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_i915_private_t *dev_priv = dev->dev_private;
	drm_i915_setparam_t param;

	if (!dev_priv) {
		DRM_ERROR("%s called with no initialization\n", __FUNCTION__);
		return DRM_ERR(EINVAL);
	}

	DRM_COPY_FROM_USER_IOCTL(param, (drm_i915_setparam_t __user *) data,
				 sizeof(param));

	switch (param.param) {
	case I915_SETPARAM_USE_MI_BATCHBUFFER_START:
		dev_priv->use_mi_batchbuffer_start = param.value;
		break;
	case I915_SETPARAM_TEX_LRU_LOG_GRANULARITY:
		dev_priv->tex_lru_log_granularity = param.value;
		break;
	case I915_SETPARAM_ALLOW_BATCHBUFFER:
		dev_priv->allow_batchbuffer = param.value;
		break;
	default:
		DRM_ERROR("unknown parameter %d\n", param.param);
		return DRM_ERR(EINVAL);
	}

	return 0;
}

/*ARGSUSED*/
int i915_driver_load(drm_device_t *dev, unsigned long flags)
{
	/* i915 has 4 more counters */
	dev->counters += 4;
	dev->types[6] = _DRM_STAT_IRQ;
	dev->types[7] = _DRM_STAT_PRIMARY;
	dev->types[8] = _DRM_STAT_SECONDARY;
	dev->types[9] = _DRM_STAT_DMA;

	return 0;
}

void i915_driver_lastclose(drm_device_t * dev)
{
	if (dev->dev_private) {
		drm_i915_private_t *dev_priv = dev->dev_private;
		i915_mem_takedown(&(dev_priv->agp_heap));
	}
	(void) i915_dma_cleanup(dev);
}

void i915_driver_preclose(drm_device_t * dev, DRMFILE filp)
{
	if (dev->dev_private) {
		drm_i915_private_t *dev_priv = dev->dev_private;
		if (dev_priv->page_flipping) {
		(void) i915_do_cleanup_pageflip(dev);
		}
		i915_mem_release(dev, filp, dev_priv->agp_heap);
	}
}

extern drm_ioctl_desc_t i915_ioctls[];

void i915_set_ioctl_desc(int n, drm_ioctl_t * func,
	    int auth_needed, int root_only, char *desc)
{
	i915_ioctls[n].func = func;
	i915_ioctls[n].auth_needed = auth_needed;
	i915_ioctls[n].root_only = root_only;
	i915_ioctls[n].desc = desc;
}
void
i915_init_ioctl_arrays(void)
{
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_INIT),
	    i915_dma_init, 1, 1, "i915_dma_init");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_FLUSH),
	    i915_flush_ioctl, 1, 0, "i915_flush_ioctl");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_FLIP),
	    i915_flip_bufs, 1, 0, "i915_flip_bufs");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_BATCHBUFFER),
	    i915_batchbuffer, 1, 0, "i915_batchbuffer");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_IRQ_EMIT),
	    i915_irq_emit, 1, 0, " i915_irq_emit");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_IRQ_WAIT),
	    i915_irq_wait, 1, 0, "i915_irq_wait");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_GETPARAM),
	    i915_getparam, 1, 0, "i915_getparam");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_SETPARAM),
	    i915_setparam, 1, 1, "i915_setparam");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_ALLOC),
	    i915_mem_alloc, 1, 0, "i915_mem_alloc");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_FREE),
	    i915_mem_free, 1, 0, "i915_mem_free");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_INIT_HEAP),
	    i915_mem_init_heap, 1, 1, "i915_mem_init_heap");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_CMDBUFFER),
	    i915_cmdbuffer, 1, 0, "i915_cmdbuffer");
	i915_set_ioctl_desc(DRM_IOCTL_NR(DRM_I915_DESTROY_HEAP),
	    i915_mem_destroy_heap, 1, 1, "i915_mem_destroy_heap");
}
/**
 * Determine if the device really is AGP or not.
 *
 * All Intel graphics chipsets are treated as AGP, even if they are really
 * PCI-e.
 *
 * \param dev   The device to be tested.
 *
 * \returns
 * A value of 1 is always retured to indictate every i9x5 is AGP.
 */
/*ARGSUSED*/
int i915_driver_device_is_agp(drm_device_t * dev)
{
	return 1;
}
