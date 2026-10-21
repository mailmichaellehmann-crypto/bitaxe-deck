cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# LVGL-Objekte und -Styles im PSRAM anlegen, damit der interne RAM für WLAN/TLS frei bleibt
idf_build_set_property(COMPILE_OPTIONS "-DLV_MEM_CUSTOM_ALLOC(s)=heap_caps_malloc(s,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DLV_MEM_CUSTOM_REALLOC(p,s)=heap_caps_realloc(p,s,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DLV_MEM_CUSTOM_FREE=heap_caps_free" APPEND)
project(bitaxe_deck)
