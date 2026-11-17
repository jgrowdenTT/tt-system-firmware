# SPDX-License-Identifier: Apache-2.0
#
# Host (native_sim) ztest for MCUboot multi-key public key handling, against
# upstream MCUboot 2.5.0. Key files are resolved and embedded the same way as
# boot/zephyr/CMakeLists.txt, and the real boot/zephyr/keys.c builds the table.

cmake_minimum_required(VERSION 3.20.0)

get_filename_component(MCUBOOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../bootloader/mcuboot" ABSOLUTE)
set(BOOT_DIR ${MCUBOOT_DIR}/boot)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mcuboot_multikey_test)

# Split the comma-separated key list and resolve each entry (as upstream does).
string(REPLACE "," ";" key_list "${CONFIG_BOOT_SIGNATURE_KEY_FILE}")
list(LENGTH key_list key_count)
set(key_index 0)
foreach(raw_key_path IN LISTS key_list)
  string(CONFIGURE "${raw_key_path}" key_path)
  if(IS_ABSOLUTE ${key_path})
    set(resolved_key_path ${key_path})
  elseif(EXISTS ${APPLICATION_CONFIG_DIR}/${key_path})
    set(resolved_key_path ${APPLICATION_CONFIG_DIR}/${key_path})
  else()
    set(resolved_key_path ${MCUBOOT_DIR}/${key_path})
  endif()

  # Entry 0 keeps the plain symbol names; later entries get an index suffix,
  # matching what keys.c expects.
  if(key_index EQUAL 0)
    set(generated_pubkey ${ZEPHYR_BINARY_DIR}/autogen-pubkey.c)
    set(name_suffix_arg "")
  else()
    set(generated_pubkey ${ZEPHYR_BINARY_DIR}/autogen-pubkey-${key_index}.c)
    set(name_suffix_arg --name-suffix _${key_index})
  endif()
  message(STATUS "MCUboot verification key #${key_index}: ${resolved_key_path}")

  add_custom_command(
    OUTPUT ${generated_pubkey}
    COMMAND ${PYTHON_EXECUTABLE} ${MCUBOOT_DIR}/scripts/imgtool.py
            getpub -k ${resolved_key_path} ${name_suffix_arg} -o ${generated_pubkey}
    DEPENDS ${resolved_key_path}
  )
  target_sources(app PRIVATE ${generated_pubkey})
  math(EXPR key_index "${key_index} + 1")
endforeach()

message(STATUS "multi-key test embeds ${key_count} keys")
target_compile_definitions(app PRIVATE MCUBOOT_SIGN_KEY_COUNT=${key_count})

# bootutil sources needed for key lookup and RSA-PSS verification.
zephyr_include_directories(
  ${BOOT_DIR}/bootutil/include
  ${BOOT_DIR}/bootutil/src
  ${BOOT_DIR}/zephyr/include
)
zephyr_sources(
  ${BOOT_DIR}/bootutil/src/bootutil_find_key.c
  ${BOOT_DIR}/bootutil/src/image_ecdsa.c
  ${BOOT_DIR}/bootutil/src/fault_injection_hardening.c
  ${BOOT_DIR}/bootutil/src/bootutil_misc.c
)

target_sources(app PRIVATE
  ${BOOT_DIR}/zephyr/keys.c
  src/main.c
)
