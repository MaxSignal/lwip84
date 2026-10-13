;; usb_ll.s - fast FIFO access for the USB controller (page 0)
;;
;; void usb_fifo_out(const uint8_t *buf, uint16_t count_port)
;; void usb_fifo_in(uint8_t *buf, uint16_t count_port)
;;   HL = buffer, D = byte count (0 = nothing), E = FIFO port (A0h + endpoint)
	.module usb_ll
	.area	_HOME

_usb_fifo_out::
	ld	c, e
	ld	a, d
	or	a, a
	ret	z
	ld	b, d
	otir
	ret

_usb_fifo_in::
	ld	c, e
	ld	a, d
	or	a, a
	ret	z
	ld	b, d
	inir
	ret
