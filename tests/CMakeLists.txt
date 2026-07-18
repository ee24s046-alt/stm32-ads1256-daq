#
# Native unit tests over the portable core.
#
# These link the *same* .c files the firmware links - firmware/lib/*.c - not a
# host reimplementation.  That is what makes them worth running: a bug fixed
# here is fixed in the image.
#
# Built with ASan + UBSan by default.  The ring buffer does pointer arithmetic
# over a caller-supplied buffer and the packet codec does a lot of shifting and
# sign extension; both are exactly the code that benefits from a sanitiser, and
# neither can run one on the target.
#
option(DAQ_SANITIZE "Build unit tests with ASan/UBSan" ON)

set(DAQ_TEST_FLAGS -Wall -Wextra -Wconversion -Wsign-conversion -g -O1)
set(DAQ_TEST_LINK "")

if(DAQ_SANITIZE AND NOT MSVC)
    list(APPEND DAQ_TEST_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    list(APPEND DAQ_TEST_LINK  -fsanitize=address,undefined)
endif()

foreach(t test_spsc_ring test_packet test_calibration)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE daq_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${DAQ_TEST_FLAGS})
    if(DAQ_TEST_LINK)
        target_link_options(${t} PRIVATE ${DAQ_TEST_LINK})
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The library itself must also be sanitised, or ASan only sees the test code.
if(DAQ_SANITIZE AND NOT MSVC)
    target_compile_options(daq_core PRIVATE -fsanitize=address,undefined)
endif()
