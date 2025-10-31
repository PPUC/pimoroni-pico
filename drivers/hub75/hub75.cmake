option(ENABLE_PICO_GRAPHICS "Option to build shared library with or without pico_graphics support" ON)

add_library(hub75 INTERFACE)

target_sources(hub75 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/hub75.cpp)

pico_generate_pio_header(hub75 ${CMAKE_CURRENT_LIST_DIR}/hub75.pio)

target_include_directories(hub75 INTERFACE ${CMAKE_CURRENT_LIST_DIR})

# Pull in pico libraries that we need
if(ENABLE_PICO_GRAPHICS)
    target_link_libraries(hub75 INTERFACE pico_stdlib hardware_pio hardware_dma pico_graphics)
else ()
    target_compile_options(hub75 INTERFACE -DNO_PICO_GRAPHICS)
    target_link_libraries(hub75 INTERFACE pico_stdlib hardware_pio hardware_dma)
endif ()
