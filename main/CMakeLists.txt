set( incs
     "."
     "led/include" )

set( srcs 
     "main.c"
     "led/led_strip_rgb.c"
     "led/led_strip_uwf.c" )

idf_component_register( SRCS "${srcs}"
                        INCLUDE_DIRS "${incs}" )
