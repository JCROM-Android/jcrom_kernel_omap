/*
 * OMAP3 Voltage Controller (VC) data
 *
 * Copyright (C) 2007, 2010 Texas Instruments, Inc.
 * Rajendra Nayak <rnayak@ti.com>
 * Lesly A M <x0080970@ti.com>
 * Thara Gopinath <thara@ti.com>
 *
 * Copyright (C) 2008, 2011 Nokia Corporation
 * Kalle Jokiniemi
 * Paul Walmsley
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/io.h>
#include <linux/err.h>
#include <linux/init.h>

#include <plat/common.h>

#include "prm-regbits-34xx.h"
#include "voltage.h"

#include "vc.h"

/*
 * VC data common to 34xx/36xx chips
 * XXX This stuff presumably belongs in the vc3xxx.c or vc.c file.
 */
static struct omap_vc_common omap3_vc_common = {
	.smps_sa_reg	 = OMAP3_PRM_VC_SMPS_SA_OFFSET,
	.smps_volra_reg	 = OMAP3_PRM_VC_SMPS_VOL_RA_OFFSET,
	.smps_cmdra_reg	 = OMAP3_PRM_VC_SMPS_CMD_RA_OFFSET,
	.bypass_val_reg	 = OMAP3_PRM_VC_BYPASS_VAL_OFFSET,
	.data_shift	 = OMAP3430_DATA_SHIFT,
	.slaveaddr_shift = OMAP3430_SLAVEADDR_SHIFT,
	.regaddr_shift	 = OMAP3430_REGADDR_SHIFT,
	.valid		 = OMAP3430_VALID_MASK,
	.cmd_on_shift	 = OMAP3430_VC_CMD_ON_SHIFT,
	.cmd_on_mask	 = OMAP3430_VC_CMD_ON_MASK,
	.cmd_onlp_shift	 = OMAP3430_VC_CMD_ONLP_SHIFT,
	.cmd_ret_shift	 = OMAP3430_VC_CMD_RET_SHIFT,
	.cmd_off_shift	 = OMAP3430_VC_CMD_OFF_SHIFT,
	.cfg_channel_reg = OMAP3_PRM_VC_CH_CONF_OFFSET,
	.i2c_cfg_hsen_mask = OMAP3430_HSEN_MASK,
	.i2c_cfg_reg	 = OMAP3_PRM_VC_I2C_CFG_OFFSET,
	.i2c_mcode_mask	 = OMAP3430_MCODE_MASK,
};

/*
 * VC auto transition settings for OMAP3. On OMAP3, we just have a single
 * device wide state that is achieved on core, so we shall use this data
 * only for core domain transition
 */
static const struct omap_vc_auto_trans omap3_vc_auto_trans = {
	.reg = OMAP3_PRM_VOLTCTRL_OFFSET,
	.sleep_val =  OMAP3430_AUTO_SLEEP_MASK,
	.retention_val = OMAP3430_AUTO_RET_MASK,
	.off_val = OMAP3430_AUTO_OFF_MASK,
};

struct omap_vc_channel omap3_vc_mpu = {
	.common = &omap3_vc_common,
	.cmdval_reg = OMAP3_PRM_VC_CMD_VAL_0_OFFSET,
	.smps_sa_mask = OMAP3430_PRM_VC_SMPS_SA_SA0_MASK,
	.smps_volra_mask = OMAP3430_VOLRA0_MASK,
	.smps_cmdra_mask = OMAP3430_CMDRA0_MASK,
	.cfg_channel_sa_shift = OMAP3430_PRM_VC_SMPS_SA_SA0_SHIFT,
};

struct omap_vc_channel omap3_vc_core = {
	.common = &omap3_vc_common,
	.cmdval_reg = OMAP3_PRM_VC_CMD_VAL_1_OFFSET,
	.smps_sa_mask = OMAP3430_PRM_VC_SMPS_SA_SA1_MASK,
	.smps_volra_mask = OMAP3430_VOLRA1_MASK,
	.smps_cmdra_mask = OMAP3430_CMDRA1_MASK,
	.cfg_channel_sa_shift = OMAP3430_PRM_VC_SMPS_SA_SA1_SHIFT,

	.auto_trans = &omap3_vc_auto_trans,
	.auto_trans_mask = OMAP3430_AUTO_OFF_MASK | OMAP3430_AUTO_RET_MASK |
				OMAP3430_AUTO_SLEEP_MASK,
};
