    .section .rodata
    .balign 64
    .global gd_ps2_ui2_begin
    .global gd_ps2_ui2_end
gd_ps2_ui2_begin:
    .incbin "ps2/out/main_menu.p2sheet"
gd_ps2_ui2_end:
    .balign 4
