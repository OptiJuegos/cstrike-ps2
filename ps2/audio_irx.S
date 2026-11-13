/* Embed the PS2SDK audsrv IOP module for standalone ELF loading.
 * The binary is from the local SDK and is not copied into our sources. */
    .section .rodata
    .balign 16
    .globl gd_ps2_audsrv_irx_begin
gd_ps2_audsrv_irx_begin:
    .incbin "psdevwindows/ps2sdk/iop/irx/audsrv.irx"
    .globl gd_ps2_audsrv_irx_end
gd_ps2_audsrv_irx_end:
    .balign 16
