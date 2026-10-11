;; farcall.s - cross-bank call trampoline used by the thunks of banker.py.
;;
;; A thunk enters with
;;   B  = flash page of the target, C = caller's A (first 8-bit argument)
;;   IY = target address (4000h-7FFFh), HL/DE = argument registers
;;   (SP) = return address of the caller
;; The caller's return address and its bank are moved to the far stack, so
;; the target sees the same stack as with a direct call.  Return values in
;; A, DE and HL are preserved on the way back.  Not reentrant from
;; interrupts: the ISR must not call banked code.
	.module farcall
	.globl	___farcall
	.globl	___farstack_init

FARSTACK_DEPTH = 96

	.area	_DATA
fc_sp:	.ds	2
fc_hl:	.ds	2
fc_de:	.ds	2
farstack:
	.ds	3 * FARSTACK_DEPTH

	.area	_HOME
___farstack_init::
	ld	hl, #farstack
	ld	(fc_sp), hl
	ret

___farcall::
	ld	(fc_hl), hl
	ld	(fc_de), de
	pop	de			; caller's return address
	ld	hl, (fc_sp)
	ld	(hl), e
	inc	hl
	ld	(hl), d
	inc	hl
	in	a, (0x06)		; caller's bank
	ld	(hl), a
	inc	hl
	ld	(fc_sp), hl
	ld	a, b
	out	(0x06), a
	ld	a, c
	ld	hl, (fc_hl)
	ld	de, (fc_de)
	call	jp_iy
	ld	(fc_hl), hl
	ld	c, a
	ld	hl, (fc_sp)
	dec	hl
	ld	a, (hl)
	out	(0x06), a
	dec	hl
	ld	b, (hl)
	dec	hl
	ld	a, (hl)
	ld	(fc_sp), hl
	ld	l, a
	ld	h, b
	push	hl			; caller's return address
	ld	a, c
	ld	hl, (fc_hl)
	ret
jp_iy:
	jp	(iy)
