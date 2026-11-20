.syntax unified
.thumb
.global _start
.extern main
.type _start, %function

_start:
	bl main
#ifdef SMS_TW_MOBILE_PROFILE
	bl _exit
#else
1:
	b 1b
#endif
