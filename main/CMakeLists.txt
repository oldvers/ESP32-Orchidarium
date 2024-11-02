set( incs
     "."
     "led/include"
     "climate/include"
     "utils/include"
     "udp/include"
     "wifi/include"
     "time/include" )

set( srcs 
     "main.c"
     "led/led_strip_rgb.c"
     "led/led_strip_uwf.c"
     "led/led_task.c"
     "climate/fan.c"
     "climate/humidifier.c"
     "utils/i2c.c"
     "udp/udp_dns_server.c"
     "wifi/wifi_task.c"
     "time/time_task.c" )

idf_component_register( SRCS "${srcs}"
                        INCLUDE_DIRS "${incs}" )
