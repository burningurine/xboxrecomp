# USB: MCPX OHCI host controllers, so a title's own XAPI can find a pad

# The controllers' registers are served through the page-protection trap in
# platform/ (mmio_trap.c), which on Windows is only needed here.
add_library(xbox_usb STATIC
    ohci.c
    usb_gamepad.c
    ../platform/mmio_trap.c
)

target_include_directories(xbox_usb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_usb PUBLIC platform xbox_input)

if(MSVC)
    target_compile_options(xbox_usb PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_usb PRIVATE -Wall -Wextra -O2)
endif()
