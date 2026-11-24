idf_component_register(
    SRCS "main.c"
         "aquarium.c"
         "aquarium_bg.c"
         "sprites.c"
         "fish_sim.c"
         "idle_policy.c"
         "ascii_font.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer
)
