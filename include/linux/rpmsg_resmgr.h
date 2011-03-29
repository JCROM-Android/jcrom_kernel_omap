/*
 * Remote processor messaging
 *
 * Copyright(c) 2011 Texas Instruments. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name Texas Instruments nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _LINUX_RPMSG_RESMGR_H
#define _LINUX_RPMSG_RESMGR_H

enum {
	RPRES_GPTIMER   = 0,
	RPRES_IVAHD     = 1,
	RPRES_IVASEQ0   = 2,
	RPRES_IVASEQ1   = 3,
	RPRES_L3BUS     = 4,
	RPRES_ISS       = 5,
};

enum {
	RPRES_REQ_FREE  = 0,
	RPRES_REQ_ALLOC = 1,
};

struct rpres_head {
	u32 proc;
	u32 res_type;
	u32 acquire;
	u32 res_id;
	u32 priv;
	u32 data_sz;
	char data[];
} __packed;

struct rpres_head_ack {
	u32 ret;
	u32 res_type;
	u32 res_id;
	u32 priv;
	u32 data_sz;
	char data[];
} __packed;

struct rpres_gpt {
	u32 base;
	u32 id;
	u32 src_clk;
};

struct rpres_iva {
	u32 perf;
	u32 lat;
};

struct rpres_l3_bus {
	u32 bw;
	u32 lat;
};

struct rpres_iss {
	u32 perf;
	u32 lat;
};

#endif /* _LINUX_RPMSG_RESMGR_H */
