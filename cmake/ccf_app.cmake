# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the Apache 2.0 License.

set(ALLOWED_TARGETS "sgx;virtual")

set(COMPILE_TARGETS
    "sgx;virtual"
    CACHE
      STRING
      "List of target compilation platforms. Choose from: ${ALLOWED_TARGETS}"
)

set(IS_VALID_TARGET "FALSE")
foreach(REQUESTED_TARGET ${COMPILE_TARGETS})
  if(${REQUESTED_TARGET} IN_LIST ALLOWED_TARGETS)
    set(IS_VALID_TARGET "TRUE")
  else()
    message(
      FATAL_ERROR
        "${REQUESTED_TARGET} is not a valid target. Choose from: ${ALLOWED_TARGETS}"
    )
  endif()
endforeach()

if((NOT ${IS_VALID_TARGET}))
  message(
    FATAL_ERROR
      "Variable list 'COMPILE_TARGETS' must include at least one supported target. Choose from: ${ALLOWED_TARGETS}"
  )
endif()

# Find OpenEnclave package
find_package(OpenEnclave 0.18.0 CONFIG REQUIRED)
# As well as pulling in openenclave:: targets, this sets variables which can be
# used for our edge cases (eg - for virtual libraries). These do not follow the
# standard naming patterns, for example use OE_INCLUDEDIR rather than
# OpenEnclave_INCLUDE_DIRS

set(OE_TARGET_LIBC openenclave::oelibc)
set(OE_TARGET_ENCLAVE_AND_STD openenclave::oeenclave openenclave::oelibcxx
                              openenclave::oelibc openenclave::oecryptoopenssl
)
# These oe libraries must be linked in specific order
set(OE_TARGET_ENCLAVE_CORE_LIBS openenclave::oeenclave openenclave::oesnmalloc
                                openenclave::oecore openenclave::oesyscall
)

option(LVI_MITIGATIONS "Enable LVI mitigations" ON)
if(LVI_MITIGATIONS)
  string(APPEND OE_TARGET_LIBC -lvi-cfg)
  list(TRANSFORM OE_TARGET_ENCLAVE_AND_STD APPEND -lvi-cfg)
  list(TRANSFORM OE_TARGET_ENCLAVE_CORE_LIBS APPEND -lvi-cfg)
endif()

function(add_lvi_mitigations name)
  if(LVI_MITIGATIONS)
    apply_lvi_mitigation(${name})
  endif()
endfunction()

if(LVI_MITIGATIONS)
  set(LVI_MITIGATION_BINDIR
      /opt/oe_lvi
      CACHE STRING "Path to the LVI mitigation bindir."
  )
  find_package(
    OpenEnclave-LVI-Mitigation CONFIG REQUIRED HINTS ${OpenEnclave_DIR}
  )
endif()

list(APPEND COMPILE_LIBCXX -stdlib=libc++)
if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 9)
  list(APPEND LINK_LIBCXX -lc++ -lc++abi -stdlib=libc++)
else()
  # Clang <9 needs to link libc++fs when using <filesystem>
  list(APPEND LINK_LIBCXX -lc++ -lc++abi -lc++fs -stdlib=libc++)
endif()

# Sign a built enclave library with oesign
function(sign_app_library name app_oe_conf_path enclave_sign_key_path)
  cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "" "INSTALL_LIBS")

  if(TARGET ${name})
    # Produce a debuggable variant. This doesn't need to be signed, but oesign
    # also stamps the other config (heap size etc) which _are_ needed
    set(DEBUG_CONF_NAME ${CMAKE_CURRENT_BINARY_DIR}/${name}.debuggable.conf)

    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.debuggable
      # Copy conf file locally
      COMMAND cp ${app_oe_conf_path} ${DEBUG_CONF_NAME}
      # Remove any existing Debug= lines
      COMMAND sed -i "/^Debug=\.*/d" ${DEBUG_CONF_NAME}
      # Add Debug=1 line
      COMMAND echo "Debug=1" >> ${DEBUG_CONF_NAME}
      COMMAND
        openenclave::oesign sign -e ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so -c
        ${DEBUG_CONF_NAME} -k ${enclave_sign_key_path} -o
        ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.debuggable
      DEPENDS ${name} ${app_oe_conf_path} ${enclave_sign_key_path}
    )

    add_custom_target(
      ${name}_debuggable ALL
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.debuggable
    )

    # Produce a releaseable signed variant. This is NOT debuggable - oegdb
    # cannot be attached
    set(SIGNED_CONF_NAME ${CMAKE_CURRENT_BINARY_DIR}/${name}.signed.conf)
    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.signed
      # Copy conf file locally
      COMMAND cp ${app_oe_conf_path} ${SIGNED_CONF_NAME}
      # Remove any existing Debug= lines
      COMMAND sed -i "/^Debug=\.*/d" ${SIGNED_CONF_NAME}
      # Add Debug=0 line
      COMMAND echo "Debug=0" >> ${SIGNED_CONF_NAME}
      COMMAND
        openenclave::oesign sign -e ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so -c
        ${SIGNED_CONF_NAME} -k ${enclave_sign_key_path}
      DEPENDS ${name} ${app_oe_conf_path} ${enclave_sign_key_path}
    )

    add_custom_target(
      ${name}_signed ALL
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.signed
    )

    if(${PARSED_ARGS_INSTALL_LIBS})
      install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.debuggable
              DESTINATION lib
      )
      install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.signed
              DESTINATION lib
      )
    endif()
  endif()
endfunction()

# Util functions used by add_ccf_app and others
function(enable_quote_code name)
  if(QUOTES_ENABLED)
    target_compile_definitions(${name} PUBLIC -DGET_QUOTE)
  endif()
endfunction()

# Enclave library wrapper
function(add_ccf_app name)

  cmake_parse_arguments(
    PARSE_ARGV 1 PARSED_ARGS "" ""
    "SRCS;INCLUDE_DIRS;LINK_LIBS_ENCLAVE;LINK_LIBS_VIRTUAL;DEPS;INSTALL_LIBS"
  )
  add_custom_target(${name} ALL)

  if("sgx" IN_LIST COMPILE_TARGETS)
    set(enc_name ${name}.enclave)

    add_library(${enc_name} SHARED ${PARSED_ARGS_SRCS})

    target_include_directories(
      ${enc_name} SYSTEM PRIVATE ${PARSED_ARGS_INCLUDE_DIRS}
    )
    add_warning_checks(${enc_name})
    target_link_libraries(
      ${enc_name} PRIVATE ${PARSED_ARGS_LINK_LIBS_ENCLAVE}
                          ${OE_TARGET_ENCLAVE_CORE_LIBS} ccf.enclave
    )

    set_property(TARGET ${enc_name} PROPERTY POSITION_INDEPENDENT_CODE ON)

    add_lvi_mitigations(${enc_name})

    add_dependencies(${name} ${enc_name})
    if(PARSED_ARGS_DEPS)
      add_dependencies(${enc_name} ${PARSED_ARGS_DEPS})
    endif()
  endif()

  if("virtual" IN_LIST COMPILE_TARGETS)
    # Build a virtual enclave, loaded as a shared library without OE
    set(virt_name ${name}.virtual)

    add_library(${virt_name} SHARED ${PARSED_ARGS_SRCS})

    target_include_directories(
      ${virt_name} SYSTEM PRIVATE ${PARSED_ARGS_INCLUDE_DIRS}
    )
    add_warning_checks(${virt_name})

    target_link_libraries(
      ${virt_name} PRIVATE ${PARSED_ARGS_LINK_LIBS_VIRTUAL} ccf.virtual
    )

    if(NOT SAN)
      target_link_options(${virt_name} PRIVATE LINKER:--no-undefined)
    endif()

    target_link_options(
      ${virt_name} PRIVATE
      LINKER:--undefined=enclave_create_node,--undefined=enclave_run
    )

    set_property(TARGET ${virt_name} PROPERTY POSITION_INDEPENDENT_CODE ON)

    add_san(${virt_name})

    add_dependencies(${name} ${virt_name})
    if(PARSED_ARGS_DEPS)
      add_dependencies(${virt_name} ${PARSED_ARGS_DEPS})
    endif()

    if(${PARSED_ARGS_INSTALL_LIBS})
      install(TARGETS ${virt_name} DESTINATION lib)
    endif()
  endif()
endfunction()

# Convenience wrapper to build C-libraries that can be linked in enclave, ie. in
# a CCF application.
function(add_enclave_library_c name)
  cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "" "")
  set(files ${PARSED_ARGS_UNPARSED_ARGUMENTS})
  add_library(${name} STATIC ${files})
  target_compile_options(${name} PRIVATE -nostdinc)
  target_link_libraries(${name} PRIVATE ${OE_TARGET_LIBC})
  set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)
endfunction()

# Convenience wrapper to build C++-libraries that can be linked in enclave, ie.
# in a CCF application.
function(add_enclave_library name)
  cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "" "")
  set(files ${PARSED_ARGS_UNPARSED_ARGUMENTS})
  add_library(${name} ${files})
  target_compile_options(${name} PUBLIC -nostdinc -nostdinc++)
  target_compile_definitions(
    ${name} PUBLIC INSIDE_ENCLAVE _LIBCPP_HAS_THREAD_API_PTHREAD
  )
  target_link_libraries(${name} PUBLIC ${OE_TARGET_ENCLAVE_AND_STD} -lgcc)
  set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)
endfunction()

function(add_host_library name)
  cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "" "")
  set(files ${PARSED_ARGS_UNPARSED_ARGUMENTS})
  add_library(${name} ${files})
  target_compile_options(${name} PUBLIC ${COMPILE_LIBCXX})
  target_link_libraries(${name} PUBLIC ${LINK_LIBCXX} -lgcc openenclave::oehost)
  set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)
endfunction()
