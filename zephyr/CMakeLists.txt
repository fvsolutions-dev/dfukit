if(CONFIG_DFUKIT)
  set(DFUKIT_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
  zephyr_library_named(dfukit)
  zephyr_library_sources(
    ${DFUKIT_DIR}/src/dfukit_server.c
    ${DFUKIT_DIR}/src/dfukit_client.c
    ${DFUKIT_DIR}/src/dfukit_inflate.c
    ${DFUKIT_DIR}/lib/crc32/dfukit_crc32.c
    ${DFUKIT_DIR}/lib/miniz/miniz_tinfl.c
  )
  zephyr_include_directories(${DFUKIT_DIR}/src ${DFUKIT_DIR}/lib/crc32 ${DFUKIT_DIR}/lib/miniz)
  # As the plain build (../CMakeLists.txt): miniz's settings, its names prefixed
  zephyr_compile_definitions(
    MINIZ_NO_MALLOC
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
    tinfl_decompress=dfukit_tinfl_decompress
    tinfl_decompress_mem_to_heap=dfukit_tinfl_decompress_mem_to_heap
    tinfl_decompress_mem_to_mem=dfukit_tinfl_decompress_mem_to_mem
    tinfl_decompress_mem_to_callback=dfukit_tinfl_decompress_mem_to_callback
  )
  set_source_files_properties(${DFUKIT_DIR}/lib/miniz/miniz_tinfl.c
    PROPERTIES COMPILE_OPTIONS -w)
  if(CONFIG_DFUKIT_MCUBOOT_RECOVERY)
    zephyr_library_sources(
      ${DFUKIT_DIR}/src/backends/mcuboot-recovery/dfukit_mcuboot.c
      ${DFUKIT_DIR}/src/backends/mcuboot-recovery/recovery.c
    )
    zephyr_include_directories(${DFUKIT_DIR}/src/backends/mcuboot-recovery)
    # Its frames on the UART: burst-link, a module of the same image
    zephyr_library_link_libraries(burst-link-protocol)
    # MCUboot's own headers (bootutil/mcuboot_status.h), for the hook
    zephyr_library_include_directories(
      ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include
      ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/zephyr/include
    )
  endif()
endif()
