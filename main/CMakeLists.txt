set( incs
     "."
     "led/include"
     "climate/include"
     "utils/include"
     "udp/include"
     "wifi/include"
     "time/include"
     "http/daemon/include"
     "http/server/include"
     "http/server/fsdata" )

set( srcs 
     "main.c"
     "led/led_strip_rgb.c"
     "led/led_strip_uwf.c"
     "led/led_task.c"
     "climate/fan.c"
     "climate/humidifier.c"
     "climate/climate_task.c"
     "utils/i2c.c"
     "udp/udp_dns_server.c"
     "wifi/wifi_task.c"
     "time/time_task.c"
     "http/daemon/fs.c"
     "http/daemon/httpd.c"
     "http/daemon/strcasestr.c"
     "http/server/http_server.c" )

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_definitions("-DLWIP_HTTPD_CGI=1")
    add_definitions("-DLWIP_HTTPD_SSI=1")
    add_definitions("-DHTTPD_DEBUG=LWIP_DBG_ON")
endif()

idf_component_register( SRCS "${srcs}"
                        INCLUDE_DIRS "${incs}" )
