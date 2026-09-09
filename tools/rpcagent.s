@ RPCAgent - the guest half of the portal.
@
@ Everything else the debugger does watches the machine from outside. This
@ module exists for the one thing that cannot be done from there: asking
@ RISC OS to do something. The host has no safe moment to call in, but a
@ module inside can call out and then act on the answer.
@
@ It ticks, asks the host whether there is work, and if there is, sets a
@ transient callback. The callback is the point of the whole arrangement:
@ ticker routines run in interrupt context where calling OS_CLI would be
@ unsound, while a callback runs in user mode at a moment the OS has chosen.
@
@ Built for ARMv4 so it runs on the StrongARM the emulator is configured as,
@ and dropped into poduleroms/ as filetype &FFA, which RISC OS initialises
@ from the expansion card ROM at every boot without being asked.

	.arch	armv4
	.text

	@ RISC OS SWIs, X form so errors return rather than abort
	XOS_CLI			= 0x20005
	XOS_Module		= 0x2001E
	@ Numbers taken from db/riscos_prm.sqlite, not from memory: CallEvery
	@ and RemoveTickerEvent are &3C and &3D, and guessing them wrong meant
	@ the module loaded, ticked never, and said nothing - the X form turns
	@ a wrong SWI into a silent error rather than a crash.
	XOS_CallEvery		= 0x2003C
	XOS_RemoveTickerEvent	= 0x2003D
	XOS_AddCallBack		= 0x20054
	XOS_Write0		= 0x20002
	XOS_NewLine		= 0x20003

	Module_Claim		= 6
	Module_Free		= 7

	@ The portal, in the same host SWI chunk as HostFS
	ARCEM_SWI_CHUNK		= 0x56AC0
	XRPCAgent		= (ARCEM_SWI_CHUNK | 0x20000) + 5

	@ Reason codes, matching src/debug/dbg_portal.c
	AGENT_HELLO		= 0
	AGENT_POLL		= 1
	AGENT_FETCH		= 2
	AGENT_DONE		= 3

	AGENT_VERSION		= 1
	WORKSPACE_SIZE		= 1024
	TICK_CENTISECONDS	= 5


	.global	_start
_start:

module_start:
	.int	0		@ Start
	.int	init		@ Initialisation
	.int	final		@ Finalisation
	.int	0		@ Service Call
	.int	title		@ Title String
	.int	help		@ Help String
	.int	table		@ Help and Command keyword table
	.int	0		@ SWI Chunk base
	.int	0		@ SWI handler code
	.int	0		@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Message File
	.int	modflags	@ Module Flags

modflags:
	.int	1		@ 32 bit compatible

title:
	.string	"RPCAgent"

help:
	.string	"RPCAgent\t1.00 (RPCEmu control portal)"
	.align

table:
	.string	"RPCAgent"
	.align
	.int	command_rpcagent
	.int	0x00000000
	.int	0
	.int	command_rpcagent_help
	.byte	0		@ Table terminator
	.align

command_rpcagent_help:
	.string	"*RPCAgent reports whether the emulator control portal is active\rSyntax: *RPCAgent"
	.align

running_text:
	.string	"RPCAgent 1.00 is loaded and talking to the host"
	.align


	@ Initialisation.
	@   r12 = pointer to this instantiation's private word
init:
	stmfd	sp!, {r0-r4, lr}

	@ Claim workspace. Module code runs in place in the expansion card
	@ ROM, which is read only, so the command buffer has to live in RAM
	@ the module owns.
	mov	r0, #Module_Claim
	mov	r3, #WORKSPACE_SIZE
	swi	XOS_Module
	bvs	init_failed
	str	r2, [r12]

	@ Announce ourselves, and hand the host the buffer it should write
	@ commands into.
	mov	r0, #AGENT_HELLO
	mov	r1, r2
	mov	r2, #AGENT_VERSION
	swi	XRPCAgent

	@ Start ticking. r2 is passed to the ticker in r12.
	mov	r0, #TICK_CENTISECONDS
	adr	r1, ticker
	ldr	r2, [r12]
	swi	XOS_CallEvery

	ldmfd	sp!, {r0-r4, lr}
	msr	cpsr_f, #0		@ V clear: no error
	mov	pc, lr

init_failed:
	@ Leave r0 alone - it is the error pointer the OS wants, and V is
	@ already set from the failed SWI.
	add	sp, sp, #4
	ldmfd	sp!, {r1-r4, pc}


	@ Finalisation
final:
	stmfd	sp!, {r0-r2, lr}

	adr	r0, ticker
	ldr	r1, [r12]
	swi	XOS_RemoveTickerEvent

	mov	r0, #Module_Free
	ldr	r2, [r12]
	swi	XOS_Module

	ldmfd	sp!, {r0-r2, lr}
	msr	cpsr_f, #0
	mov	pc, lr


	@ Ticker, called from interrupt context every TICK_CENTISECONDS.
	@   r12 = workspace pointer
	@
	@ Nothing may be done here that assumes a sane OS context, so this
	@ only asks whether there is work and, if so, arranges to be called
	@ back somewhere it is safe to act.
ticker:
	stmfd	sp!, {r0-r2, lr}

	mov	r0, #AGENT_POLL
	swi	XRPCAgent
	cmp	r0, #0
	beq	ticker_done

	adr	r0, callback
	mov	r1, r12
	swi	XOS_AddCallBack

ticker_done:
	ldmfd	sp!, {r0-r2, pc}


	@ Transient callback, called in user mode at a point the OS chose.
	@   r12 = workspace pointer
	@
	@ This is where the work actually happens, and the only context in
	@ which calling OS_CLI is legitimate.
callback:
	stmfd	sp!, {r0-r3, lr}

	@ Ask the host to write the command into our workspace.
	mov	r0, #AGENT_FETCH
	mov	r1, r12
	swi	XRPCAgent
	cmp	r0, #0
	beq	callback_done

	mov	r0, r12
	swi	XOS_CLI

	@ Report what happened. V set means OS_CLI returned an error, and r0
	@ then points at the error block - pass it on so the host can say what
	@ went wrong rather than only that something did.
	mov	r2, r0
	mov	r1, #0
	movvs	r1, #1
	mov	r0, #AGENT_DONE
	swi	XRPCAgent

callback_done:
	ldmfd	sp!, {r0-r3, pc}


	@ *RPCAgent - proof of life, for a human at the prompt
command_rpcagent:
	stmfd	sp!, {r0, lr}
	adr	r0, running_text
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {r0, lr}
	msr	cpsr_f, #0
	mov	pc, lr
