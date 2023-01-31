if (CONFIG_USE_BMP180)
    set(srcs "bmp180.c")
endif()
idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "include")


