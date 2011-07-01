/*
 * OMAP44xx secure APIs file.
 *
 * Copyright (C) 2010 Texas Instruments, Inc.
 * Written by Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 *
 * This program is free software,you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/linkage.h>

/*
 * This is common routine to manage secure monitor API
 * used to modify the PL310 secure registers.
 * 'r0' contains the value to be modified and 'r12' contains
 * the monitor API number. It uses few CPU registers
 * internally and hence they need be backed up including
 * link register "lr".
 * Function signature : void omap_smc1(u32 fn, u32 arg)
 */

ENTRY(omap_smc1)
	stmfd   sp!, {r2-r12, lr}
	mov	r12, r0
	mov 	r0, r1
	dsb
	smc	#0
	ldmfd   sp!, {r2-r12, pc}
ENDPROC(omap_smc1)

/*
 * Low level common routine to manage secure
 * HAL APIs.
 * Function signature : u32 omap_smc2(u32 id, u32 falg, u32 pargs)
 * @id : Application ID of HAL APIs
 * @flag : Flag to indicate the criticality of operation
 * @pargs : Physical address of parameter list starting
 *	    with number of parametrs
 */
ENTRY(omap_smc2)
	stmfd   sp!, {r1-r12, lr}
	mov	r3, r2
	mov	r2, r1
	mov	r1, #0x0	@ Process ID
	mov	r6, #0xff
	mov	r12, #0x00	@ Secure Service ID
	mov	r7, #0
	mcr	p15, 0, r7, c7, c5, 6
	dsb
	dmb
	smc	#0
	ldmfd   sp!, {r1-r12, pc}
END(omap_smc2)

ENTRY(omap_modify_auxcoreboot0)
	stmfd   sp!, {r1-r12, lr}
	ldr	r12, =0x104
	dsb
	smc	#0
	ldmfd   sp!, {r1-r12, pc}
ENDPROC(omap_modify_auxcoreboot0)

ENTRY(omap_auxcoreboot_addr)
	stmfd   sp!, {r2-r12, lr}
	ldr	r12, =0x105
	dsb
	smc	#0
	ldmfd   sp!, {r2-r12, pc}
ENDPROC(omap_auxcoreboot_addr)

ENTRY(omap_read_auxcoreboot0)
	stmfd   sp!, {r2-r12, lr}
	ldr	r12, =0x103
	dsb
	smc	#0
	mov	r0, r0, lsr #9
	ldmfd   sp!, {r2-r12, pc}
ENDPROC(omap_read_auxcoreboot0)

/******************************************************************************/

#define SMICODEPUB_IRQ_END   0xFE
#define SMICODEPUB_FIQ_END   0xFD
#define SMICODEPUB_RPC_END   0xFC

#define PUB2SEC_NOCST        0xFF
#define SMICODEPUB_NEWTASK   0x00

/*
 * RPC status:
 *  - 0: the secure world yielded due to an interrupt
 *  - 1: the secure world yielded on an RPC (no public thread is handling it)
 *  - 2: the secure world yielded on an RPC and the response is ready
 */
#define RPC_ADVANCEMENT_NONE		0
#define RPC_ADVANCEMENT_PENDING		1
#define RPC_ADVANCEMENT_FINISHED	2

#ifdef CONFIG_ARM_ERRATA_430973
#define INVALIDATE_BTB       MCR p15, 0, R0, c7, c5, 6
#else
#define INVALIDATE_BTB
#endif

schedule_secure_world:
	.global schedule_secure_world

	/* Save registers */
	push	{r4-r12, lr}

	/* Copy the Secure Service ID in r12 */
	mov     r12, r0

	cmp	r0, #SMICODEPUB_IRQ_END
	beq	return_from_irq

	cmp	r0, #SMICODEPUB_RPC_END
	beq	return_from_rpc

	mov	r6, #PUB2SEC_NOCST
	mov	r12, #SMICODEPUB_NEWTASK

	b	label_smc

return_from_rpc:
	ldr	r9, =g_RPC_parameters
	ldm	r9, {r0-r3}
	/* fall through */

return_from_irq:
	ldr	r10, =g_secure_task_id
	ldr	r6, [r10]

	b	label_smc

label_smc:
	INVALIDATE_BTB
	dsb
	dmb

	smc	#0
	b	service_end
	nop

	INVALIDATE_BTB
	ldr	r8, =g_secure_task_id
	str	r6, [r8]

	mov	r0, #0x00
	ldr	r8, =g_service_end
	str	r0, [r8]

	b	schedule_secure_world_exit

service_end:

schedule_secure_world_exit:

	INVALIDATE_BTB

	/* Restore registers */
	pop	{r4-r12, pc}

rpc_handler:
	.global rpc_handler

	INVALIDATE_BTB

	/* g_RPC_advancement = RPC_ADVANCEMENT_PENDING */
	ldr	r8, =g_RPC_advancement
	mov	r9, #RPC_ADVANCEMENT_PENDING
	str	r9, [r8]

	ldr	r8, =g_RPC_parameters
	stm	r8, {r0-r3}

	ldr	r8, =g_secure_task_id
	str	r6, [r8]

	mov	r0, #0x00
	ldr	r8, =g_service_end
	str	r0, [r8]

	/* Restore registers */
	pop	{r4-r12, pc}
