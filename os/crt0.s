;; crt0.s - reset entry, OS header and startup for the TI-84 Plus port.
;;
;; Memory map (mode 0):
;;   0000-3FFF  flash page 0: this file, common code, thunks, constants
;;   4000-7FFF  bank A: flash page of the running banked module (port 06h)
;;   8000-BFFF  RAM page 1 (port 07h = 81h)
;;   C000-FFFF  RAM page 0 (port 05h = 00h); stack grows down from FFFFh
	.module crt0
	.globl	_main
	.globl	_isr
	.globl	___farstack_init

	.area	_HEADER (ABS)
	.org	0x0000
	jp	init
	.org	0x0038
	jp	isr_entry
	.org	0x0053
	jp	init			; entry used by the boot code
	.org	0x0056
	.db	0xFF, 0xA5, 0xFF	; "OS valid" marker
	.org	0x0064
	.ascii	"lwIP-84+"
	.db	0

	;; Area order for the linker (_CODE is linked at 0080h).
	.area	_CODE
	.area	_HOME
	.area	_CONST
	.area	_INITIALIZER
	.area	_GSINIT
	.area	_GSFINAL
	.area	_DATA
	.area	_INITIALIZED
	.area	_BSEG
	.area	_BSS
	.area	_HEAP

	.area	_HOME
init:
	di
	im	1
	ld	a, #0x06		; memory map mode 0, timer speed bits 11
	out	(0x04), a
	ld	a, #0x81		; RAM page 1 at 8000h
	out	(0x07), a
	xor	a			; RAM page 0 at C000h
	out	(0x05), a
	out	(0x03), a		; no interrupt sources yet
	ld	a, #0x01		; 15 MHz (ignored on the plain TI-83 Plus)
	out	(0x20), a
	ld	sp, #0x0000
	;; clear all of RAM 8000h-FFFFh (the stack is still empty)
	ld	hl, #0x8000
	ld	de, #0x8001
	ld	bc, #0x7FFF
	ld	(hl), #0
	ldir
	call	gsinit
	call	___farstack_init
	call	_main
halt_forever:
	di
	halt
	jr	halt_forever

;; Interrupt entry (IM 1).  Saves everything SDCC may clobber and calls the
;; C handler, which lives in page 0.
isr_entry:
	push	af
	push	bc
	push	de
	push	hl
	push	ix
	push	iy
	ex	af, af'
	exx
	push	af
	push	bc
	push	de
	push	hl
	call	_isr
	pop	hl
	pop	de
	pop	bc
	pop	af
	exx
	ex	af, af'
	pop	iy
	pop	ix
	pop	hl
	pop	de
	pop	bc
	pop	af
	ei
	ret

	.area	_GSINIT
gsinit::
	;; _DATA is already zero (whole RAM cleared above)
	ld	bc, #l__INITIALIZER
	ld	a, b
	or	a, c
	jr	Z, gsinit_next
	ld	de, #s__INITIALIZED
	ld	hl, #s__INITIALIZER
	ldir
gsinit_next:
	.area	_GSFINAL
	ret
