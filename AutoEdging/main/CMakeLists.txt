idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES app)

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
