cmake_minimum_required(VERSION 3.16)

# dfukit lives two levels above this example directory.
# examples/dfu_example_app → examples → dfukit (the component directory).
# EXTRA_COMPONENT_DIRS must point at the *parent* of the component — i.e.
# the directory that CONTAINS dfukit/ — so IDF picks up dfukit as a
# named component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dfu_example_app)

# Inject our project-owned tusb_config.h onto espressif__tinyusb's include
# path so upstream TinyUSB's "#include <tusb_config.h>" resolves here.
idf_component_get_property(_tinyusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${_tinyusb_lib} PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/main")
