# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Ed25519 signatures for a catalogue, and the text form of a key, a signature
# and a fingerprint. Monocypher is compiled unchanged, with its warnings off,
# one translation unit per vendored file: the two files define helpers of the
# same names.
set(oa_monocypher_dir "${PROJECT_SOURCE_DIR}/third_party/monocypher")
add_library(oa-base-signing-monocypher OBJECT src/monocypher_core.c src/monocypher_ed25519.c)
add_library(oa::base::signing_monocypher ALIAS oa-base-signing-monocypher)
target_include_directories(oa-base-signing-monocypher SYSTEM PRIVATE ${oa_monocypher_dir})
if(MSVC)
  # /W0 turns every warning off. oa-options is left out: an option given after
  # the warning level it sets would not replace that level.
  target_compile_options(oa-base-signing-monocypher PRIVATE /W0)
else()
  target_link_libraries(oa-base-signing-monocypher PRIVATE oa-options)
  target_compile_options(oa-base-signing-monocypher PRIVATE -w)
endif()

add_library(oa-base-signing src/ed25519.cpp src/base64.cpp src/sealed_key.cpp)
add_library(oa::base::signing ALIAS oa-base-signing)
target_include_directories(oa-base-signing PUBLIC include)
target_include_directories(oa-base-signing SYSTEM PRIVATE ${oa_monocypher_dir})
target_compile_features(oa-base-signing PUBLIC cxx_std_20)
target_link_libraries(oa-base-signing PUBLIC oa-options
  PRIVATE oa-base-sha256 oa-base-signing-monocypher)
if(BUILD_TESTING)
  add_executable(oa-base-signing-test tests/ed25519_test.cpp)
  target_link_libraries(oa-base-signing-test PRIVATE oa-base-signing oa-test-support oa-options)
  add_test(NAME base-signing COMMAND oa-base-signing-test)
  set_tests_properties(base-signing PROPERTIES TIMEOUT 30)

  # The sealed key calls valid_key_id, which the registry defines. This test
  # links the registry after signing so that reference resolves. The signing
  # library itself does not link the registry: the registry already links
  # signing, and a base library does not link data.
  add_executable(oa-base-signing-sealed-key-test tests/sealed_key_test.cpp)
  target_include_directories(oa-base-signing-sealed-key-test SYSTEM PRIVATE ${oa_monocypher_dir})
  target_link_libraries(oa-base-signing-sealed-key-test PRIVATE oa-base-signing oa-data-registry
    oa-test-support oa-options)
  # Keep that order when the rest of the tree reorders direct libraries.
  set_property(TARGET oa-base-signing-sealed-key-test PROPERTY LINK_LIBRARIES_STRATEGY REORDER_MINIMALLY)
  add_test(NAME base-signing-sealed-key COMMAND oa-base-signing-sealed-key-test)
  set_tests_properties(base-signing-sealed-key PROPERTIES TIMEOUT 60)
endif()
