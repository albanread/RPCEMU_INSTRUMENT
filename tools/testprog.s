@ A deliberately misbehaving RISC OS Absolute image, for exercising the
@ debugger: it loops, writes repeatedly to a fixed address, calls a
@ subroutine, and faults inside it with the return address on the stack.
@
@ ARMv4 only, so it runs on the StrongARM the emulator is configured as.
@ Loaded and entered at &8000 as filetype &FF8.

	.arch	armv4
	.text
	.global	_start

_start:
	@ RISC OS enters an Absolute image without a usable stack pointer - it
	@ arrives as &80000000, and the first push takes a data abort. The
	@ program sets up its own, above the address the watchpoint watches.
	mov	sp, #0xa000
	mov	r2, #0x9000		@ the address a watchpoint is set on
	mov	r3, #0

counting_loop:
	add	r3, r3, #1
	str	r3, [r2]		@ the write a watchpoint catches
	cmp	r3, #5
	blt	counting_loop

	bl	crashing
	mov	pc, lr			@ not reached

crashing:
	str	lr, [sp, #-4]!		@ return address on the stack, for the backtrace
	mov	r0, #1			@ recognisable register values
	mov	r1, #0x2a
	.word	0xe7f000f0		@ permanently undefined instruction
	ldr	lr, [sp], #4
	mov	pc, lr
