# SPDX-License-Identifier: GPL-3.0-or-later
# Curated, unmodified OpenWurli DSP headers from reales/retromulator:
# bb7ffd871c7c227aed0671ba39df8188a7a4697d (GPL v3).
# No synthLib device, JUCE, editor, samples, ROMs or manufacturer patches.
set(EMULATION_WURLI_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(EMULATION_OPENWURLI_DIR "${EMULATION_WURLI_ROOT}/3rdparty/openwurli")
if(NOT EXISTS "${EMULATION_OPENWURLI_DIR}/owVoice.h")
  message(FATAL_ERROR "Missing the curated pinned OpenWurli DSP source in 3rdparty/openwurli.")
endif()
add_library(score_emulation_wurli STATIC "${EMULATION_WURLI_ROOT}/Emulation/Wurli/Engine.cpp")
target_compile_features(score_emulation_wurli PUBLIC cxx_std_20)
target_include_directories(score_emulation_wurli PUBLIC "${EMULATION_WURLI_ROOT}" PRIVATE "${EMULATION_OPENWURLI_DIR}")
set_target_properties(score_emulation_wurli PROPERTIES
  POSITION_INDEPENDENT_CODE ON UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(score_emulation_wurli PRIVATE -fno-fast-math)
endif()
if(UNIX)
  target_link_libraries(score_emulation_wurli PRIVATE m)
endif()
set(EMULATION_WURLI_NATIVE_SOURCES
  "${EMULATION_WURLI_ROOT}/Emulation/Wurli/Controls.cpp"
  "${EMULATION_WURLI_ROOT}/Emulation/Wurli/ProcessModel.cpp"
  "${EMULATION_WURLI_ROOT}/Emulation/Wurli/ProcessModelSerialization.cpp"
  "${EMULATION_WURLI_ROOT}/Emulation/Wurli/ProcessFactory.cpp"
  "${EMULATION_WURLI_ROOT}/Emulation/Wurli/Layer.cpp"
  "${EMULATION_WURLI_ROOT}/Emulation/Wurli/Executor/Component.cpp"
  "${EMULATION_WURLI_ROOT}/Emulation/Wurli/Executor/Node.cpp")
if(COMMAND score_register_license)
  score_register_license(OpenWurli
    URL "https://github.com/reales/retromulator/tree/bb7ffd871c7c227aed0671ba39df8188a7a4697d/source/openWurliLib"
    FILES "${EMULATION_OPENWURLI_DIR}/NOTICE.txt" "${EMULATION_OPENWURLI_DIR}/LICENSE.txt")
endif()
