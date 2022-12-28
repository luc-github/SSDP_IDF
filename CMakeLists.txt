set(srcs "ssdp.c")
set(dependencies lwip console esp_netif esp_timer)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${dependencies}
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")

if(CONFIG_ETH_ENABLED)
    idf_component_optional_requires(PRIVATE esp_eth)
endif()
