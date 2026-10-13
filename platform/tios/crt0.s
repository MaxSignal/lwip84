;; crt0.s - start-up of the TI-OS applications (make PLATFORM=tios).
;;
;; TI-OS starts the app on its first flash page (platform/tios/launch.s, bank A).
;; The launcher saves TI-OS's RAM pages 80h and 81h in 86h and 87h, copies
;; the common code, the constants and the initializers (linked from 8184h,
;; the RAM image) to RAM and jumps to app_init here.  Memory map while the
;; app runs (mode 0):
;;   0000-3FFF  TI-OS flash page 0 (not used)
;;   4000-7FFF  bank A: the app's flash page of the running banked module
;;   8000-80FF  IM 2 vector table (I = 80h, every entry 8181h)
;;   8181-8183  jp isr_entry
;;   8184-      this file, common code, thunks, constants, initializers,
;;              data, heap; stack grows down from FFFFh
;; RAM page 81h (8000-BFFF) is where TI-OS lets code run (ports 25h/26h),
;; so the code has to come first.  Banked pages are numbered from the
;; app's first page down: logical page n is flash page (base - n), see
;; platform/tiplatform/os/farcall.s.
	.module crt0_app
	.globl	_main
	.globl	_isr
	.globl	___farstack_init
	.globl	___app_save
	.globl	___app_exit

	;; Area order for the linker (_CODE is linked at 8184h).
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
;; filled in by the launcher before app_init:
;;   +0 base page, +1..2 TI-OS SP, +3 port 03h, +4 port 04h, +5 port 05h,
;;   +6 port 07h, +7 port 20h, +8 I, +9 TI-OS's contrast (8447h; main.c
;;   puts the app's back here before the exit)
___app_save::
	.ds	10

app_init::
	ld	sp, #0x0000
	;; IM 2: a table of 257 bytes 81h at 8000h, jp isr_entry at 8181h
	ld	hl, #0x8000
	ld	de, #0x8001
	ld	bc, #0x0100
	ld	(hl), #0x81
	ldir
	ld	a, #0xC3
	ld	(0x8181), a
	ld	hl, #isr_entry
	ld	(0x8182), hl
	ld	a, #0x80
	ld	i, a
	im	2
	ld	a, #0x06		; memory map mode 0, timer speed bits 11
	out	(0x04), a
	xor	a
	out	(0x03), a		; no interrupt sources yet
	ld	a, #0x01		; 15 MHz
	out	(0x20), a
	;; clear the data (all RAM after the image; the stack is still empty)
	ld	hl, #0xFFFF
	ld	de, #s__DATA
	or	a
	sbc	hl, de			; bytes after the first
	ld	b, h
	ld	c, l
	ld	hl, #s__DATA
	ld	(hl), #0
	ld	d, h
	ld	e, l
	inc	de
	ldir
	call	gsinit
	call	___farstack_init
	call	_main
	;; main does not return; leave to TI-OS if it does
	jp	___app_exit

;; back to TI-OS: the launcher page restores its RAM and ports
___app_exit::
	di
	ld	a, (___app_save)
	out	(0x06), a
	jp	0x4103			; app_launch.s: exit

;; Interrupt entry (IM 2 through 8181h).
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
	reti

	;; start of the heap: the _HEAP area is linked after all other data
	.area	_HEAP
_heap_base::

	.area	_GSINIT
gsinit::
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
