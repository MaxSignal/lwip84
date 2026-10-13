;; launch.s - first page of a TI-OS application (platform/tios/): the app header,
;; the start (TI-OS jumps to 4080h with this page in bank A) and the way
;; back.  The RAM image (platform/tiplatform/os/crt0.s) follows at 4400h, placed there by
;; tools/mkapp84.py, which also fills in image_len.
;;
;; Start: save TI-OS's RAM (page 81h -> 87h, 80h -> 86h), its SP and the
;; ports that change, copy the image to 8184h and go to app_init.  Exit:
;; the same backwards, IY = flags (89F0h) as TI-OS needs it everywhere,
;; then back to TI-OS's home screen.  Interrupts are
;; off throughout: the stack is not usable while the pages move.
	.module app_launch
	.globl	app_init
	.globl	___app_save
	.globl	s__CODE

	.area	_APPHDR (ABS)
	.org	0x4000
	.db	0x80, 0x0F, 0, 0, 0, 0		; length (rabbitsign)
	.db	0x80, 0x12, 0x01, 0x04		; key 0104 (shareware)
	.db	0x80, 0x21, 0x01		; revision
	.db	0x80, 0x31, 0x01		; build
	.db	0x80, 0x48			; name, 8 characters (mkapp84.py --name)
	.ascii	"lwIP84  "
	.db	0x80, 0x81, 0x01		; pages (rabbitsign)
	.db	0x80, 0x90			; no splash screen
	.db	0x03, 0x26, 0x09, 0x04, 0x04, 0x6F, 0x1B, 0x80	; date stamp
	.db	0x02, 0x0D, 0x40		; dummy date stamp signature
	.db	0xA1, 0x6B, 0x99, 0xF6, 0x59, 0xBC, 0x67, 0xF5
	.db	0x85, 0x9C, 0x09, 0x6C, 0x0F, 0xB4, 0x03, 0x9B
	.db	0xC9, 0x03, 0x32, 0x2C, 0xE0, 0x03, 0x20, 0xE3
	.db	0x2C, 0xF4, 0x2D, 0x73, 0xB4, 0x27, 0xC4, 0xA0
	.db	0x72, 0x54, 0xB9, 0xEA, 0x7C, 0x3B, 0xAA, 0x16
	.db	0xF6, 0x77, 0x83, 0x7A, 0xEE, 0x1A, 0xD4, 0x42
	.db	0x4C, 0x6B, 0x8B, 0x13, 0x1F, 0xBB, 0x93, 0x8B
	.db	0xFC, 0x19, 0x1C, 0x3C, 0xEC, 0x4D, 0xE5, 0x75
	.db	0x80, 0x7F, 0, 0, 0, 0		; program image
	.org	0x4080
	jp	launch
image_len::
	.dw	0				; tools/mkapp84.py
	.org	0x4103
	jp	leave				; ___app_exit (platform/tiplatform/os/crt0.s)

launch:
	di
	ld	ix, #0
	add	ix, sp				; TI-OS's stack
	ld	a, i				; I in A'
	ex	af, af'
	ld	a, (0x8447)			; TI-OS's contrast, in IYH
	ld	iy, #0
	ld	c, a
	ld	b, #0
	add	iy, bc
	in	a, (0x06)			; this page: the base
	exx
	ld	b, a
	in	a, (0x03)
	ld	c, a
	in	a, (0x04)
	ld	d, a
	in	a, (0x05)
	ld	e, a
	in	a, (0x07)
	ld	h, a
	in	a, (0x20)
	ld	l, a
	exx
	;; 48 KiB of RAM: pages 82h-87h are one page, and 86h would be 87h;
	;; look with the first byte of 86h and 87h through the C000h window
	;; (the code runs in bank A), and put it back
	ld	a, #0x06
	out	(0x05), a
	ld	a, (0xC000)
	ld	c, a				; 86h:0000
	ld	a, #0x07
	out	(0x05), a
	ld	a, (0xC000)
	ld	b, a				; 87h:0000
	ld	a, #0x55
	ld	(0xC000), a
	ld	a, #0x06
	out	(0x05), a
	ld	a, #0xAA
	ld	(0xC000), a
	ld	a, #0x07
	out	(0x05), a
	ld	a, (0xC000)
	ld	d, a
	ld	a, b
	ld	(0xC000), a
	ld	a, #0x06
	out	(0x05), a
	ld	a, c
	ld	(0xC000), a
	exx
	ld	a, e				; TI-OS's page at C000h
	exx
	out	(0x05), a
	ld	a, d
	cp	#0xAA
	jr	NZ, ram128
	;; not for this calculator: back to TI-OS untouched
	ld	sp, ix
	ex	af, af'
	ld	i, a
	ld	iy, #0x89F0			; TI-OS's flags: its ISR uses IY
	ei
	call	0x0050				; B_JUMP _JForceCmdNoChar
	.dw	0x4027
ram128:
	;; RAM page 81h (at 8000h) to 87h (at C000h); the stack is gone now
	ld	a, #0x07
	out	(0x05), a
	ld	hl, #0x8000
	ld	de, #0xC000
	ld	bc, #0x4000
	ldir
	;; page 80h (mapped at 8000h) to 86h
	ld	a, #0x80
	out	(0x07), a
	ld	a, #0x06
	out	(0x05), a
	ld	hl, #0x8000
	ld	de, #0xC000
	ld	bc, #0x4000
	ldir
	ld	a, #0x81
	out	(0x07), a
	xor	a
	out	(0x05), a
	;; the image
	ld	hl, #0x4400
	ld	de, #s__CODE
	ld	bc, (image_len)
	ldir
	exx
	ld	a, b
	ld	(___app_save + 0), a
	ld	a, c
	ld	(___app_save + 3), a
	ld	a, d
	ld	(___app_save + 4), a
	ld	a, e
	ld	(___app_save + 5), a
	ld	a, h
	ld	(___app_save + 6), a
	ld	a, l
	ld	(___app_save + 7), a
	exx
	ld	(___app_save + 1), ix
	ex	af, af'
	ld	(___app_save + 8), a
	push	iy				; the stack is ours again
	pop	bc
	ld	a, c
	ld	(___app_save + 9), a
	jp	app_init

leave:
	di
	ld	ix, (___app_save + 1)
	ld	a, (___app_save + 9)		; the app's contrast, in IY
	ld	c, a
	ld	b, #0
	ld	iy, #0
	add	iy, bc
	ld	a, (___app_save + 8)
	ex	af, af'
	exx
	ld	a, (___app_save + 10)		; variables to put into TI-OS
	ld	b, a
	ld	a, (___app_save + 3)
	ld	c, a
	ld	a, (___app_save + 4)
	ld	d, a
	ld	a, (___app_save + 5)
	ld	e, a
	ld	a, (___app_save + 6)
	ld	h, a
	ld	a, (___app_save + 7)
	ld	l, a
	exx
	;; page 87h (at C000h) back to 81h (at 8000h)
	ld	a, #0x07
	out	(0x05), a
	ld	hl, #0xC000
	ld	de, #0x8000
	ld	bc, #0x4000
	ldir
	;; 86h back to 80h (mapped at 8000h)
	ld	a, #0x80
	out	(0x07), a
	ld	a, #0x06
	out	(0x05), a
	ld	hl, #0xC000
	ld	de, #0x8000
	ld	bc, #0x4000
	ldir
	exx
	ld	a, h
	out	(0x07), a
	ld	a, e
	out	(0x05), a
	ld	a, d
	out	(0x04), a
	ld	a, l
	out	(0x20), a
	ld	a, c
	out	(0x03), a
	exx
	ld	sp, ix
	ex	af, af'
	ld	i, a
	im	1
	;; the contrast as the app left it, also for TI-OS (8447h), and the
	;; LCD modes TI-OS expects: 8-bit, X increment
	push	iy
	pop	bc
	ld	a, c
	ld	(0x8447), a
	add	a, #0xD8
	call	lcd_cmd
	ld	a, #0x01
	call	lcd_cmd
	ld	a, #0x05
	call	lcd_cmd
	ld	iy, #0x89F0			; TI-OS's flags: its ISR and bcalls use IY
	ei
	exx
	ld	a, b
	exx
	or	a
	call	NZ, install
	call	0x0050				; B_JUMP _JForceCmdNoChar
	.dw	0x4027

;; The variables the browser downloaded (app/browser/browser.c): a table
;; at 4000h of RAM page 85h, count then 13 bytes each: type, name (8),
;; bytes with the size word (2), where they are (2, 4000h-7FFFh of page
;; 85h).  Each replaces a variable of that name if there is room for it.
INST_COPY = 0x9872			; appBackUpScreen: code may run there
INST_TAB = 0x86EC			; saveSScreen: the table
INST_CUR = 0x9872 + 0x40		; the entry being done
INST_LEFT = 0x9872 + 0x42		; entries left
OP1 = 0x8478

install:
	ld	hl, #xcopy
	ld	de, #INST_COPY
	ld	bc, #xcopy_end - xcopy
	ldir
	ld	hl, #0x4000
	ld	de, #INST_TAB
	ld	bc, #256
	call	INST_COPY
	ld	a, (INST_TAB)
	or	a
	ret	Z
	ld	(INST_LEFT), a
	ld	hl, #INST_TAB + 1
	ld	(INST_CUR), hl
inst_loop:
	call	inst_op1
	rst	0x28
	.dw	0x42F1				; _ChkFindSym
	jr	C, inst_new
	rst	0x28
	.dw	0x4FC6				; _DelVarArc
inst_new:
	call	inst_op1
	ld	hl, (INST_CUR)
	ld	de, #9
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)				; bytes with the size word
	ex	de, hl
	push	hl
	ld	de, #24				; name and symbol table entry
	add	hl, de
	rst	0x28
	.dw	0x42FD				; _EnoughMem: carry if not
	pop	hl
	jr	C, inst_next
	dec	hl
	dec	hl				; the variable's size
	ld	a, (OP1)
	cp	#0x15
	jr	Z, inst_appvar
	cp	#0x06
	jr	Z, inst_prot
	rst	0x28
	.dw	0x4339				; _CreateProg
	jr	inst_copy
inst_prot:
	rst	0x28
	.dw	0x4E6D				; _CreateProtProg
	jr	inst_copy
inst_appvar:
	rst	0x28
	.dw	0x4E6A				; _CreateAppVar
inst_copy:
	;; DE: the new variable's size word; size word and data from page 85h
	ld	hl, (INST_CUR)
	ld	bc, #9
	add	hl, bc
	ld	c, (hl)
	inc	hl
	ld	b, (hl)
	inc	hl
	ld	a, (hl)
	inc	hl
	ld	h, (hl)
	ld	l, a
	call	INST_COPY
inst_next:
	ld	hl, (INST_CUR)
	ld	de, #13
	add	hl, de
	ld	(INST_CUR), hl
	ld	hl, #INST_LEFT
	dec	(hl)
	jr	NZ, inst_loop
	ret

inst_op1:
	ld	hl, (INST_CUR)
	ld	de, #OP1
	ld	bc, #9
	ldir
	xor	a
	ld	(de), a				; the name ends there at the latest
	ret

;; copied to INST_COPY: HL in page 85h (at 4000h) -> DE, BC bytes
xcopy:
	di
	in	a, (0x06)
	push	af
	ld	a, #0x85
	out	(0x06), a
	ldir
	pop	af
	out	(0x06), a
	ei
	ret
xcopy_end:

lcd_cmd:
	ld	b, a
lcd_busy:
	in	a, (0x10)
	rla
	jr	C, lcd_busy
	ld	a, b
	out	(0x10), a
	ret
