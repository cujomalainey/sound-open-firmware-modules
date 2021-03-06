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

#include <errno.h>
#include <reef/stream.h>
#include <reef/ssp.h>
#include <reef/alloc.h>
#include <reef/interrupt.h>

/* tracing */
#define trace_ssp(__e)	trace_event(TRACE_CLASS_SSP, __e)
#define trace_ssp_error(__e)	trace_error(TRACE_CLASS_SSP, __e)
#define tracev_ssp(__e)	tracev_event(TRACE_CLASS_SSP, __e)

/* save SSP context prior to entering D3 */
static int ssp_context_store(struct dai *dai)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);

	ssp->sscr0 = ssp_read(dai, SSCR0);
	ssp->sscr1 = ssp_read(dai, SSCR1);
	ssp->psp = ssp_read(dai, SSPSP);

	return 0;
}

/* restore SSP context after leaving D3 */
static int ssp_context_restore(struct dai *dai)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);

	ssp_write(dai, SSCR0, ssp->sscr0);
	ssp_write(dai, SSCR1, ssp->sscr1);
	ssp_write(dai, SSPSP, ssp->psp);

	return 0;
}

/* Digital Audio interface formatting */
static inline int ssp_set_config(struct dai *dai, struct dai_config *dai_config)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);
	uint32_t sscr0, sscr1, sspsp, sfifott;

	spin_lock(&ssp->lock);

	/* is playback/capture already running */
	if (ssp->state[DAI_DIR_PLAYBACK] > SSP_STATE_IDLE ||
		ssp->state[DAI_DIR_CAPTURE] > SSP_STATE_IDLE) {
		trace_ssp_error("wsS");
		goto out;
	}

	trace_ssp("SsC");

	/* reset SSP settings */
	sscr0 = 0;
	sscr1 = 0;
	sspsp = 0;
	dai->config = *dai_config;

	/* clock masters */
	switch (dai->config.ssp->format & SOF_DAI_FMT_MASTER_MASK) {
	case SOF_DAI_FMT_CBM_CFM:
		sscr1 |= SSCR1_SCLKDIR | SSCR1_SFRMDIR;
		break;
	case SOF_DAI_FMT_CBS_CFS:
		sscr1 |= SSCR1_SCFR | SSCR1_RWOT;
		break;
	case SOF_DAI_FMT_CBM_CFS:
		sscr1 |= SSCR1_SFRMDIR;
		break;
	case SOF_DAI_FMT_CBS_CFM:
		sscr1 |= SSCR1_SCLKDIR | SSCR1_SFRMDIR | SSCR1_SCFR;
		break;
	case SSP_CLK_DEFAULT:
		break;
	default:
		return -EINVAL;
	}

	/* clock signal polarity */
	switch (dai->config.ssp->format & SOF_DAI_FMT_INV_MASK) {
	case SOF_DAI_FMT_NB_NF:
		break;
	case SOF_DAI_FMT_NB_IF:
		break;
	case SOF_DAI_FMT_IB_IF:
		sspsp |= SSPSP_SCMODE(2);
		break;
	case SOF_DAI_FMT_IB_NF:
		sspsp |= SSPSP_SCMODE(2) | SSPSP_SFRMP;
		break;
	default:
		return -EINVAL;
	}

	/* clock source */
	switch (dai->config.ssp->clk_id) {
	case SSP_CLK_AUDIO:
		sscr0 |= SSCR0_ACS;
		break;
	case SSP_CLK_NET_PLL:
		sscr0 |= SSCR0_MOD;
		break;
	case SSP_CLK_EXT:
		sscr0 |= SSCR0_ECS;
		break;
	case SSP_CLK_NET:
		sscr0 |= SSCR0_NCS | SSCR0_MOD;
		break;
	default:
		return -ENODEV;
	}

	/* BCLK is generated from MCLK */
	sscr0 |= SSCR0_SCR(dai->config.ssp->mclk / dai->config.ssp->bclk - 1);

	/* format */
	switch (dai->config.ssp->format & SOF_DAI_FMT_FORMAT_MASK) {
	case SOF_DAI_FMT_I2S:
		sscr0 |= SSCR0_PSP;
		sscr1 |= SSCR1_TRAIL;
		sspsp |= SSPSP_SFRMWDTH(dai->config.ssp->frame_width + 1);
		sspsp |= SSPSP_SFRMDLY((dai->config.ssp->frame_width + 1) * 2);
		sspsp |= SSPSP_DMYSTRT(1);
		break;
	case SOF_DAI_FMT_DSP_A:
		sspsp |= SSPSP_FSRT;
	case SOF_DAI_FMT_DSP_B:
		sscr0 |= SSCR0_PSP;
		sscr1 |= SSCR1_TRAIL;
		break;
	default:
		return -EINVAL;
	}

	/* sample size */
	if (dai->config.ssp->frame_width > 16)
		sscr0 |= (SSCR0_EDSS | SSCR0_DSIZE(dai->config.ssp->frame_width - 16));
	else
		sscr0 |= SSCR0_DSIZE(dai->config.ssp->frame_width);

	/* watermarks - TODO: do we still need old sscr1 method ?? */
	sscr1 |= (SSCR1_TX(4) | SSCR1_RX(4));

	/* watermarks - (RFT + 1) should equal DMA SRC_MSIZE */
	sfifott = (SFIFOTT_TX(8) | SFIFOTT_RX(8));
#if 0
	if (dai->config.lbm)
		sscr1 |= SSCR1_LBM;
	else
		sscr1 &= ~SSCR1_LBM;
#endif
	trace_ssp("SSC");
	ssp_write(dai, SSCR0, sscr0);
	ssp_write(dai, SSCR1, sscr1);
	ssp_write(dai, SSPSP, sspsp);
	ssp_write(dai, SFIFOTT, sfifott);

	ssp->state[DAI_DIR_PLAYBACK] = SSP_STATE_IDLE;
	ssp->state[DAI_DIR_CAPTURE] = SSP_STATE_IDLE;

out:
	spin_unlock(&ssp->lock);

	return 0;
}

/* Digital Audio interface formatting */
static inline int ssp_set_loopback_mode(struct dai *dai, uint32_t lbm)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);

	trace_ssp("SLb");
	spin_lock(&ssp->lock);

	ssp_update_bits(dai, SSCR1, SSCR1_LBM, lbm ? SSCR1_LBM : 0);

	spin_unlock(&ssp->lock);

	return 0;
}

/* start the SSP for either playback or capture */
static void ssp_start(struct dai *dai, int direction)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);

	spin_lock(&ssp->lock);

	/* enable port */
	ssp_update_bits(dai, SSCR0, SSCR0_SSE, SSCR0_SSE);
	ssp->state[direction] = SSP_STATE_RUNNING;

	trace_ssp("SEn");

	/* enable DMA */
	if (direction == DAI_DIR_PLAYBACK)
		ssp_update_bits(dai, SSCR1, SSCR1_TSRE, SSCR1_TSRE);
	else
		ssp_update_bits(dai, SSCR1, SSCR1_RSRE, SSCR1_RSRE);

	spin_unlock(&ssp->lock);
}

/* stop the SSP port stream DMA and disable SSP port if no users */
static void ssp_stop(struct dai *dai, int direction)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);
	uint32_t sscr1;

	spin_lock(&ssp->lock);

	trace_ssp("SDc");

	/* disable DMA */
	if (direction == DAI_DIR_PLAYBACK) {
		if (ssp->state[DAI_DIR_PLAYBACK] == SSP_STATE_DRAINING)
			ssp_update_bits(dai, SSCR1, SSCR1_TSRE, 0);
	} else
		ssp_update_bits(dai, SSCR1, SSCR1_RSRE, 0);

	/* disable port if no users */
	sscr1 = ssp_read(dai, SSCR1);
	if (!(sscr1 & (SSCR1_TSRE | SSCR1_RSRE))) {
		ssp_update_bits(dai, SSCR0, SSCR0_SSE, 0);
		trace_ssp("SDp");
	}

	ssp->state[direction] = SSP_STATE_IDLE;

	spin_unlock(&ssp->lock);
}

static void ssp_pause(struct dai *dai, int direction)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);

	spin_lock(&ssp->lock);

	trace_ssp("SDp");

	/* disable DMA */
	if (direction == DAI_DIR_PLAYBACK) {
		if (ssp->state[DAI_DIR_PLAYBACK] == SSP_STATE_PAUSING)
			ssp_update_bits(dai, SSCR1, SSCR1_TSRE, 0);
	} else
		ssp_update_bits(dai, SSCR1, SSCR1_RSRE, 0);

	ssp->state[direction] = SSP_STATE_PAUSED;

	spin_unlock(&ssp->lock);
}

static uint32_t ssp_drain_work(void *data, uint32_t udelay)
{
	struct dai *dai = (struct dai *)data;
	struct ssp_pdata *ssp = dai_get_drvdata(dai);

	trace_ssp("SDw");

	if (ssp->state[SOF_IPC_STREAM_CAPTURE] == SSP_STATE_DRAINING)
		ssp_stop(dai, SOF_IPC_STREAM_CAPTURE);
	else
		ssp_pause(dai, SOF_IPC_STREAM_CAPTURE);
	wait_completed(&ssp->drain_complete);
	return 0;
}

static int ssp_trigger(struct dai *dai, int cmd, int direction)
{
	struct ssp_pdata *ssp = dai_get_drvdata(dai);

	trace_ssp("STr");

	switch (cmd) {
	case DAI_TRIGGER_START:
/* let's only wait until draining finished(timout) before another start */
#if 0
		/* cancel any scheduled work */
		if (ssp->state[direction] == SSP_STATE_DRAINING)
			work_cancel_default(&ssp->work);
#endif
		if (ssp->state[direction] == SSP_STATE_IDLE)
			ssp_start(dai, direction);
		break;
	case DAI_TRIGGER_PAUSE_RELEASE:
/* let's only wait until pausing finished(timout) before next release */
#if 0
		if (ssp->state[direction] == SSP_STATE_PAUSING)
			work_cancel_default(&ssp->work);
#endif
		if (ssp->state[direction] == SSP_STATE_PAUSED)
			ssp_start(dai, direction);
		break;
	case DAI_TRIGGER_PAUSE_PUSH:
		if (ssp->state[direction] != SSP_STATE_RUNNING) {
			trace_ssp_error("wsP");
			return 0;
		}
		if (direction == SOF_IPC_STREAM_CAPTURE) {
			ssp->state[SOF_IPC_STREAM_CAPTURE] =
				SSP_STATE_PAUSING;
			/* make sure the maximum 256 bytes are drained */
			work_schedule_default(&ssp->work, 1333);
			wait_init(&ssp->drain_complete);
			ssp->drain_complete.timeout = 1500;
			wait_for_completion_timeout(&ssp->drain_complete);
		} else
			ssp_pause(dai, direction);
		break;
	case DAI_TRIGGER_STOP:
		if (ssp->state[direction] != SSP_STATE_RUNNING &&
			ssp->state[direction] != SSP_STATE_PAUSED) {
			trace_ssp_error("wsO");
			return 0;
		}
		if (direction == SOF_IPC_STREAM_PLAYBACK &&
			ssp->state[direction] == SSP_STATE_RUNNING) {
			ssp->state[SOF_IPC_STREAM_PLAYBACK] =
				SSP_STATE_DRAINING;
			work_schedule_default(&ssp->work, 2000);
			wait_init(&ssp->drain_complete);
			ssp->drain_complete.timeout = 3000;
			wait_for_completion_timeout(&ssp->drain_complete);
		} else
			ssp_stop(dai, direction);
		break;
	case DAI_TRIGGER_RESUME:
		ssp_context_restore(dai);
		ssp_start(dai, direction);
		break;
	case DAI_TRIGGER_SUSPEND:
		ssp_stop(dai, direction);
		ssp_context_store(dai);
		break;
	default:
		break;
	}

	return 0;
}

static int ssp_probe(struct dai *dai)
{
	struct ssp_pdata *ssp;

	/* allocate private data */
	ssp = rzalloc(RZONE_SYS, RFLAGS_NONE, sizeof(*ssp));
	dai_set_drvdata(dai, ssp);

	work_init(&ssp->work, ssp_drain_work, dai, WORK_ASYNC);
	spinlock_init(&ssp->lock);

	ssp->state[DAI_DIR_PLAYBACK] = SSP_STATE_INIT;
	ssp->state[DAI_DIR_CAPTURE] = SSP_STATE_INIT;

	return 0;
}

const struct dai_ops ssp_ops = {
	.trigger		= ssp_trigger,
	.set_config		= ssp_set_config,
	.pm_context_store	= ssp_context_store,
	.pm_context_restore	= ssp_context_restore,
	.probe			= ssp_probe,
	.set_loopback_mode	= ssp_set_loopback_mode,
};
