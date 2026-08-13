idf_component_register(
    SRCS
        "ui.c"
        "ui_helpers.c"
        "components/ui_comp_hook.c"
        "screens/ui_Screen1.c"

    INCLUDE_DIRS
        "."
        "components"
        "screens"

    REQUIRES
        lvgl
)