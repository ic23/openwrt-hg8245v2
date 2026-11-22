/* SPDX-License-Identifier: GPL-2.0-only */
/* Stock bootm hands control to this kernel at 0x90008000, with MMU and
 * data cache off. The second stock container carries zImage + aligned DTB.
 * Read from NOR regardless of where bootm relocated its ramdisk copy.
 */
	.syntax unified
	.arm
	.text
	.global _start
_start:
	ldr	r3, =0x201e0094
	ldr	r4, =0x91000000
	mov	r5, r4
	ldr	r6, =PAYLOAD_WORDS
1:	ldr	r7, [r3], #4
	str	r7, [r4], #4
	subs	r6, r6, #1
	bne	1b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 4
	ldr	r2, =DTB_OFFSET
	add	r2, r5, r2
	bx	r5
	.ltorg
