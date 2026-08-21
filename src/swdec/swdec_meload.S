	.set noreorder

#include "pspstub.s"

	STUB_START "swdec_meload",0x40090000,0x00040005
	STUB_FUNC  0x51265768,swdec_me_boot
	STUB_FUNC  0xA91E710E,swdec_me_stop
	STUB_FUNC  0xE1A958A5,swdec_me_fault
	STUB_FUNC  0xB80050EB,swdec_me_restore
	STUB_END
