/*
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Intel Corporation nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Keyon Jie <yang.jie@linux.intel.com>
 */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <reef/reef.h>
#include <reef/lock.h>
#include <reef/list.h>
#include <reef/stream.h>
#include <reef/alloc.h>
#include <reef/trace.h>
#include <reef/dma.h>
#include <reef/ipc.h>
#include <reef/wait.h>
#include <reef/audio/component.h>
#include <reef/audio/pipeline.h>
#include <platform/dma.h>
#include <arch/cache.h>
#include <uapi/ipc.h>

#define trace_host(__e)	trace_event(TRACE_CLASS_HOST, __e)
#define tracev_host(__e)	tracev_event(TRACE_CLASS_HOST, __e)
#define trace_host_error(__e)	trace_error(TRACE_CLASS_HOST, __e)

struct hc_buf {
	/* host buffer info */
	struct list_item elem_list;
	struct list_item *current;
	uint32_t current_end;
};

struct host_data {
	/* local DMA config */
	struct dma *dma;
	int chan;
	struct dma_sg_config config;
	completion_t complete;
	struct comp_buffer *dma_buffer;
	int period_count;

	/* local and host DMA buffer info */
	struct hc_buf host;
	struct hc_buf local;
	uint32_t host_size;
	/* host possition reporting related */
	volatile uint32_t *host_pos;    /* read/write pos, update to mailbox for host side */
	uint32_t report_period; 	/* host_pos report/update to host side period, in bytes */
	uint32_t report_pos;		/* position in current report period */
	uint32_t local_pos;		/* the host side buffer local read/write possition, in bytes */
	/* pointers set during params to host or local above */
	struct hc_buf *source;
	struct hc_buf *sink;
	uint32_t split_remaining;
	uint32_t next_inc;

	/* stream info */
	struct stream_params params;
	struct sof_ipc_stream_posn posn; /* TODO: update this */
};

static inline struct dma_sg_elem *next_buffer(struct hc_buf *hc)
{
	struct dma_sg_elem *elem;

	if (list_item_is_last(hc->current, &hc->elem_list))
		elem = list_first_item(&hc->elem_list, struct dma_sg_elem, list);
	else
		elem = list_first_item(hc->current, struct dma_sg_elem, list);

	hc->current = &elem->list;
	return elem;
}

/*
 * Host period copy between DSP and host DMA completion.
 * This is called  by DMA driver every time when DMA completes its current
 * transfer between host and DSP. The host memory is not guaranteed to be
 * continuous and also not guaranteed to have a period/buffer size that is a
 * multiple of the DSP period size. This means we must check we do not
 * overflow host period/buffer/page boundaries on each transfer and split the
 * DMA transfer if we do overflow.
 */
static void host_dma_cb(void *data, uint32_t type, struct dma_sg_elem *next)
{
	struct comp_dev *dev = (struct comp_dev *)data;
	struct host_data *hd = comp_get_drvdata(dev);
	struct dma_sg_elem *local_elem, *source_elem, *sink_elem;
	struct comp_buffer *dma_buffer;
	uint32_t next_size, need_copy = 0;

	local_elem = list_first_item(&hd->config.elem_list,
		struct dma_sg_elem, list);

	trace_host("CpC");

	/* update buffer positions */
	dma_buffer = hd->dma_buffer;

	if (hd->params.pcm->direction == SOF_IPC_STREAM_PLAYBACK) {
		dma_buffer->w_ptr += local_elem->size;

		/* invalidate audio data */
		dcache_invalidate_region(dma_buffer->w_ptr, local_elem->size);

		if (dma_buffer->w_ptr >= dma_buffer->end_addr)
			dma_buffer->w_ptr = dma_buffer->addr;
#if 0
		trace_value((uint32_t)(hd->dma_buffer->w_ptr - hd->dma_buffer->addr));
#endif

		/* recalc available buffer space */
		comp_update_buffer_produce(hd->dma_buffer);
	} else {
		dma_buffer->r_ptr += local_elem->size;

		if (dma_buffer->r_ptr >= dma_buffer->end_addr)
			dma_buffer->r_ptr = dma_buffer->addr;
#if 0
		trace_value((uint32_t)(hd->dma_buffer->r_ptr - hd->dma_buffer->addr));
#endif

		/* writeback audio data */
		dcache_writeback_region(dma_buffer->r_ptr, local_elem->size);

		/* recalc available buffer space */
		comp_update_buffer_consume(hd->dma_buffer);
	}

	/* new local period, update host buffer position blks */
	hd->local_pos += local_elem->size;

	/* buffer overlap ? */
	if (hd->local_pos >= hd->host_size)
		hd->local_pos = 0;

	/* send IPC message to driver if needed */
	hd->report_pos += local_elem->size;
	if (hd->report_pos >= hd->report_period) {
		hd->report_pos = 0;
		/* update for host side */
		if (hd->host_pos) {
			*hd->host_pos = hd->local_pos;
			ipc_stream_send_notification(dev, &hd->posn);
		}
	}

	/* update src and dest positions and check for overflow */
	local_elem->src += local_elem->size;
	local_elem->dest += local_elem->size;
	if (local_elem->src == hd->source->current_end) {
		/* end of elem, so use next */
		source_elem = next_buffer(hd->source);
		hd->source->current_end = source_elem->src + source_elem->size;
		local_elem->src = source_elem->src;
	}
	if (local_elem->dest == hd->sink->current_end) {
		/* end of elem, so use next */
		sink_elem = next_buffer(hd->sink);
		hd->sink->current_end = sink_elem->dest + sink_elem->size;
		local_elem->dest = sink_elem->dest;
	}

	/* calc size of next transfer */
	next_size = dev->period_bytes;
	if (local_elem->src + next_size > hd->source->current_end)
		next_size = hd->source->current_end - local_elem->src;
	if (local_elem->dest + next_size > hd->sink->current_end)
		next_size = hd->sink->current_end - local_elem->dest;

	/* are we dealing with a split transfer ? */
	if (!hd->split_remaining) {
		/* no, is next transfer split ? */
		if (next_size != dev->period_bytes)
			hd->split_remaining = dev->period_bytes - next_size;
	} else {
		/* yes, than calc transfer size */
		need_copy = 1;
		next_size = next_size < hd->split_remaining ?
			next_size : hd->split_remaining;
		hd->split_remaining -= next_size;
	}
	local_elem->size = next_size;

	/* schedule immediate split transfer if needed */
	if (need_copy) {
		next->src = local_elem->src;
		next->dest = local_elem->dest;
		next->size = local_elem->size;
		return;
	}

	/* let any waiters know we have completed */
	wait_completed(&hd->complete);
}

static struct comp_dev *host_new(struct sof_ipc_comp *comp)
{
	struct comp_dev *dev;
	struct host_data *hd;
	struct sof_ipc_comp_host *host;
	struct sof_ipc_comp_host *ipc_host = (struct sof_ipc_comp_host *)comp;
	struct dma_sg_elem *elem;

	trace_host("new");

	dev = rzalloc(RZONE_RUNTIME, RFLAGS_NONE,
		COMP_SIZE(struct sof_ipc_comp_host));
	if (dev == NULL)
		return NULL;

	host = (struct sof_ipc_comp_host *)&dev->comp;
	memcpy(host, ipc_host, sizeof(struct sof_ipc_comp_host));

	hd = rzalloc(RZONE_RUNTIME, RFLAGS_NONE, sizeof(*hd));
	if (hd == NULL) {
		rfree(dev);
		return NULL;
	}

	elem = rzalloc(RZONE_RUNTIME, RFLAGS_NONE, sizeof(*elem));
	if (elem == NULL) {
		rfree(dev);
		rfree(hd);
		return NULL;
	}

	comp_set_drvdata(dev, hd);
	comp_set_endpoint(dev);

	hd->dma = dma_get(DMA_ID_DMAC0);
	if (hd->dma == NULL)
		goto error;

	/* init buffer elems */
	list_init(&hd->config.elem_list);
	list_init(&hd->host.elem_list);
	list_init(&hd->local.elem_list);
	list_item_prepend(&elem->list, &hd->config.elem_list);

	/* get DMA channel from DMAC0 */
	hd->chan = dma_channel_get(hd->dma);
	if (hd->chan < 0) {
		trace_host_error("eDC");
		goto error;
	}

	/* set up callback */
	dma_set_cb(hd->dma, hd->chan, DMA_IRQ_TYPE_LLIST, host_dma_cb, dev);

	return dev;

error:
	rfree(elem);
	rfree(hd);
	rfree(dev);
	return NULL;
}

static void host_free(struct comp_dev *dev)
{
	struct host_data *hd = comp_get_drvdata(dev);
	struct dma_sg_elem *elem;

	elem = list_first_item(&hd->config.elem_list,
		struct dma_sg_elem, list);
	dma_channel_put(hd->dma, hd->chan);

	rfree(elem);
	rfree(hd);
	rfree(dev);
}

static int create_local_elems(struct comp_dev *dev,
	struct stream_params *params)
{
	struct host_data *hd = comp_get_drvdata(dev);
	struct dma_sg_elem *e;
	struct list_item *elist, *tlist;
	int i;

	for (i = 0; i < hd->period_count; i++) {
		/* allocate new host DMA elem and add it to our list */
		e = rzalloc(RZONE_RUNTIME, RFLAGS_NONE, sizeof(*e));
		if (e == NULL)
			goto unwind;

		if (params->pcm->direction == SOF_IPC_STREAM_PLAYBACK)
			e->dest = (uint32_t)(hd->dma_buffer->addr) +
				i * dev->period_bytes;
		else
			e->src = (uint32_t)(hd->dma_buffer->addr) +
				i * dev->period_bytes;

		e->size = dev->period_bytes;

		list_item_append(&e->list, &hd->local.elem_list);
	}

	return 0;

unwind:
	list_for_item_safe(elist, tlist, &hd->local.elem_list) {

		e = container_of(elist, struct dma_sg_elem, list);
		list_item_del(&e->list);
		rfree(e);
	}
	return -ENOMEM;
}

static int host_elements_reset(struct comp_dev *dev)
{
	struct host_data *hd = comp_get_drvdata(dev);
	struct dma_sg_elem *source_elem, *sink_elem, *local_elem;

	/* setup elem to point to first source elem */
	source_elem = list_first_item(&hd->source->elem_list,
		struct dma_sg_elem, list);
	hd->source->current = &source_elem->list;
	hd->source->current_end = source_elem->src + source_elem->size;

	/* setup elem to point to first sink elem */
	sink_elem = list_first_item(&hd->sink->elem_list,
		struct dma_sg_elem, list);
	hd->sink->current = &sink_elem->list;
	hd->sink->current_end = sink_elem->dest + sink_elem->size;

	/* local element */
	local_elem = list_first_item(&hd->config.elem_list,
		struct dma_sg_elem, list);
	local_elem->dest = sink_elem->dest;
	local_elem->size = dev->period_bytes;
	local_elem->src = source_elem->src;
	hd->next_inc = dev->period_bytes;

	return 0;
}

/* configure the DMA params and descriptors for host buffer IO */
static int host_params(struct comp_dev *dev, struct stream_params *params)
{
	struct host_data *hd = comp_get_drvdata(dev);
	struct dma_sg_config *config = &hd->config;
	int err;

	/* set params */
	hd->params = *params;

	/* determine source and sink buffer elems */
	if (params->pcm->direction == SOF_IPC_STREAM_PLAYBACK) {

		/* set sink buffer params */
		comp_buffer_sink_params(dev, params);

		hd->source = &hd->host;
		hd->sink = &hd->local;
		hd->dma_buffer = list_first_item(&dev->bsink_list,
			struct comp_buffer, source_list);

		config->direction = DMA_DIR_HMEM_TO_LMEM;
	} else {

		/* set source buffer params */
		comp_set_source_params(dev, params);

		hd->source = &hd->local;
		hd->sink = &hd->host;
		hd->dma_buffer = list_first_item(&dev->bsource_list,
			struct comp_buffer, sink_list);

		config->direction = DMA_DIR_LMEM_TO_HMEM;
	}

	hd->period_count = hd->dma_buffer->size / dev->period_bytes;

	/* resize the buffer if space is available to align with period size */
	if (hd->period_count * dev->period_bytes <= hd->dma_buffer->alloc_size)
		hd->dma_buffer->size = hd->period_count * dev->period_bytes;
	else {
		trace_host_error("eSz");
		return -EINVAL;
	}


	/* component buffer size must be divisor of host buffer size */
	if (hd->host_size % dev->period_bytes) {
		trace_comp_error("eHB");
		trace_value(hd->host_size);
		trace_value(dev->period_bytes);
		return -EINVAL;
	}

	/* create SG DMA elems for local DMA buffer */
	err = create_local_elems(dev, params);
	if (err < 0)
		return err;

	hd->dma_buffer->r_ptr = hd->dma_buffer->addr;
	hd->dma_buffer->w_ptr = hd->dma_buffer->addr;

	/* set up DMA configuration */
	config->src_width = sizeof(uint32_t);
	config->dest_width = sizeof(uint32_t);
	config->cyclic = 0;

	host_elements_reset(dev);
	return 0;
}

/* preload the local buffers with available host data before start */
static int host_preload(struct comp_dev *dev)
{
#if 0
	struct host_data *hd = comp_get_drvdata(dev);
	int ret = 0, i;

	trace_host("PrL");

	/* preload all periods */
	for (i = 0; i < dev->preload; i++) {
		/* do DMA transfer */
		wait_init(&hd->complete);
		dma_set_config(hd->dma, hd->chan, &hd->config);
		dma_start(hd->dma, hd->chan);

		/* wait for DMA to finish */
		hd->complete.timeout = PLATFORM_DMA_TIMEOUT;
		ret = wait_for_completion_timeout(&hd->complete);
		if (ret < 0) {
			trace_comp_error("eHp");
			break;
		}
	}

	return ret;
#endif
	return 0;
}

static int host_prepare(struct comp_dev *dev)
{
	struct host_data *hd = comp_get_drvdata(dev);
	struct comp_buffer *dma_buffer;

	if (hd->params.pcm->direction == SOF_IPC_STREAM_PLAYBACK)
		dma_buffer = list_first_item(&dev->bsink_list,
			struct comp_buffer, source_list);
	else
		dma_buffer = list_first_item(&dev->bsource_list,
			struct comp_buffer, sink_list);
	dma_buffer->r_ptr = dma_buffer->w_ptr = dma_buffer->addr;

	hd->local_pos = 0;
	if (hd->host_pos)
		*hd->host_pos = 0;
	hd->report_pos = 0;
	hd->report_period = hd->params.pcm->period_bytes;
	hd->split_remaining = 0;

	//dev->preload = PLAT_HOST_PERIODS;

	dev->state = COMP_STATE_PREPARE;
	return 0;
}

static int host_pointer_reset(struct comp_dev *dev)
{
	struct host_data *hd = comp_get_drvdata(dev);

	/* reset buffer pointers */
	if (hd->host_pos)
		*hd->host_pos = 0;
	hd->local_pos = 0;
	hd->report_pos = 0;

	return 0;
}

static int host_stop(struct comp_dev *dev)
{
	/* reset host side buffer pointers */
	host_pointer_reset(dev);

	/* reset elements, to let next start from original one */
	host_elements_reset(dev);

	/* now reset downstream buffer */
	comp_buffer_reset(dev);

	dev->state = COMP_STATE_SETUP;
	return 0;
}

/* used to pass standard and bespoke commands (with data) to component */
static int host_cmd(struct comp_dev *dev, int cmd, void *data)
{
	int ret = 0;

	// TODO: align cmd macros.
	switch (cmd) {
	case COMP_CMD_PAUSE:
		/* only support pausing for running, channel is paused by DAI */
		if (dev->state == COMP_STATE_RUNNING)
			dev->state = COMP_STATE_PAUSED;
		break;
	case COMP_CMD_STOP:
		if (dev->state == COMP_STATE_RUNNING ||
			dev->state == COMP_STATE_DRAINING ||
			dev->state == COMP_STATE_PAUSED)
			ret = host_stop(dev);
		break;
	case COMP_CMD_RELEASE:
		/* channel is released by DAI */
		dev->state = COMP_STATE_RUNNING;
		break;
	case COMP_CMD_START:
		dev->state = COMP_STATE_RUNNING;
		break;
	case COMP_CMD_SUSPEND:
	case COMP_CMD_RESUME:
		break;
	default:
		break;
	}

	return ret;
}

static int host_buffer(struct comp_dev *dev, struct dma_sg_elem *elem,
		uint32_t host_size)
{
	struct host_data *hd = comp_get_drvdata(dev);
	struct dma_sg_elem *e;

	/* allocate new host DMA elem and add it to our list */
	e = rzalloc(RZONE_RUNTIME, RFLAGS_NONE, sizeof(*e));
	if (e == NULL)
		return -ENOMEM;

	*e = *elem;
	hd->host_size = host_size;

	list_item_append(&e->list, &hd->host.elem_list);
	return 0;
}

static int host_reset(struct comp_dev *dev)
{
	struct host_data *hd = comp_get_drvdata(dev);
	struct dma_sg_elem *e;
	struct list_item *elist, *tlist;

	/* free all host DMA elements */
	list_for_item_safe(elist, tlist, &hd->host.elem_list) {

		e = container_of(elist, struct dma_sg_elem, list);
		list_item_del(&e->list);
		rfree(e);
	}

	/* free all local DMA elements */
	list_for_item_safe(elist, tlist, &hd->local.elem_list) {

		e = container_of(elist, struct dma_sg_elem, list);
		list_item_del(&e->list);
		rfree(e);
	}

	host_pointer_reset(dev);
	hd->host_pos = NULL;

	hd->report_period = 0;
	hd->source = NULL;
	hd->sink = NULL;
	dev->state = COMP_STATE_INIT;

	return 0;
}

/* copy and process stream data from source to sink buffers */
static int host_copy(struct comp_dev *dev)
{
	struct host_data *hd = comp_get_drvdata(dev);
	int ret;

	trace_host("CpS");
	if (dev->state != COMP_STATE_RUNNING)
		return 0;

	/* do DMA transfer */
	wait_init(&hd->complete);
	dma_set_config(hd->dma, hd->chan, &hd->config);
	dma_start(hd->dma, hd->chan);

	/* wait for DMA to finish */
	hd->complete.timeout = PLATFORM_DMA_TIMEOUT;
	ret = wait_for_completion_timeout(&hd->complete);
	if (ret < 0)
		trace_comp_error("eHc");

	return 0;
}
struct comp_driver comp_host = {
	.type	= SOF_COMP_HOST,
	.ops	= {
		.new		= host_new,
		.free		= host_free,
		.params		= host_params,
		.reset		= host_reset,
		.cmd		= host_cmd,
		.copy		= host_copy,
		.prepare	= host_prepare,
		.preload	= host_preload,
		.host_buffer	= host_buffer,
	},
};

void sys_comp_host_init(void)
{
	comp_register(&comp_host);
}
