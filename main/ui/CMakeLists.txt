SET(SOURCES screens/ui_Screen1.c
    screens/ui_Screen2.c
    screens/ui_Screen3.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    fonts/ui_font_chinese_minifont.c
    fonts/ui_font_chines_large.c
    fonts/ui_font_chines_small.c
    fonts/ui_font_large_num.c)

add_library(ui ${SOURCES})
