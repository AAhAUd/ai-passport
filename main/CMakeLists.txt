idf_component_register(
    SRCS "main.c"
         "aquarium.c"
         "fish_sim.c"
         "ascii_font.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer
)
