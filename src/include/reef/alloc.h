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

#ifndef __INCLUDE_ALLOC__
#define __INCLUDE_ALLOC__

#include <string.h>
#include <stdint.h>
#include <reef/dma.h>

struct reef;

/* Heap Memory Zones
 *
 * The heap has three different zones from where memory can be allocated :-
 *
 * 1) System  Zone. Fixed size heap where alloc always succeeds and is never
 * freed. Used by any init code that will never give up the memory.
 *
 * 2) Runtime Zone. Main and larger heap zone where allocs are not guaranteed to
 * succeed. Memory can be freed here.
 *
 * 3) Buffer Zone. Largest heap zone intended for audio buffers.
 *
 * See platform/memory.h for heap size configuration and mappings.
 */
#define RZONE_SYS		0
#define RZONE_RUNTIME	1
#define RZONE_BUFFER	2

/*
 * Heap allocation memory flags.
 */
#define RFLAGS_NONE		0
#define RFLAGS_USED		1
#define RFLAGS_ATOMIC	2   /* allocation with IRQs off */
#define RFLAGS_DMA		4   /* DMA-able memory */
#define RFLAGS_POWER	8   /* low power memory */

struct mm_info {
	uint32_t used;
	uint32_t free;
};

/* heap allocation and free */
void *rmalloc(int zone, int flags, size_t bytes);
void *rzalloc(int zone, int flags, size_t bytes);
void rfree(void *ptr);

/* heap allocation and free for buffers on 1k boundary */
void *rballoc(int zone, int flags, size_t bytes);
void rbfree(void *ptr);

/* utility */
void bzero(void *s, size_t n);
void *memset(void *s, int c, size_t n);

/* Heap save/restore contents and context for PM D0/D3 events */
uint32_t mm_pm_context_size(void);
int mm_pm_context_save(struct dma_sg_config *sg);
int mm_pm_context_restore(struct dma_sg_config *sg);

/* heap initialisation */
void init_heap(struct reef *reef);
#endif
