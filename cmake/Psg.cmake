# SPDX-License-Identifier: GPL-3.0-or-later
# Pins: emu2149 441f3dc295a9b82db16921461f0678b483db8304 (MIT)
# Nuked-PSG d15a168c676f4669e23660be9225b34ad7c1764e (GPL-2.0-or-later).
# Select only the audited current cores; no deprecated core, ROM or asset data.
set(EMULATION_PSG_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(EMULATION_EMU2149_DIR "${EMULATION_PSG_ROOT}/3rdparty/emu2149")
set(EMULATION_NUKED_PSG_DIR "${EMULATION_PSG_ROOT}/3rdparty/Nuked-PSG")
if(NOT EXISTS "${EMULATION_EMU2149_DIR}/emu2149.c" OR NOT EXISTS "${EMULATION_NUKED_PSG_DIR}/ympsg.c")
  message(FATAL_ERROR "PSG requires the pinned emu2149 and Nuked-PSG submodules. Run git submodule update --init --recursive.")
endif()
add_library(score_emulation_psg STATIC
  "${EMULATION_PSG_ROOT}/Emulation/Psg/Engine.cpp"
  "${EMULATION_EMU2149_DIR}/emu2149.c"
  "${EMULATION_NUKED_PSG_DIR}/ympsg.c")
target_compile_features(score_emulation_psg PUBLIC cxx_std_20)
target_include_directories(score_emulation_psg PUBLIC "${EMULATION_PSG_ROOT}"
  PRIVATE "${EMULATION_EMU2149_DIR}" "${EMULATION_NUKED_PSG_DIR}")
set_target_properties(score_emulation_psg PROPERTIES
  C_STANDARD 99 POSITION_INDEPENDENT_CODE ON UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
if(UNIX)
  target_link_libraries(score_emulation_psg PRIVATE m)
endif()
set(EMULATION_PSG_NATIVE_SOURCES
  "${EMULATION_PSG_ROOT}/Emulation/Psg/Controls.cpp"
  "${EMULATION_PSG_ROOT}/Emulation/Psg/ProcessModel.cpp"
  "${EMULATION_PSG_ROOT}/Emulation/Psg/ProcessModelSerialization.cpp"
  "${EMULATION_PSG_ROOT}/Emulation/Psg/ProcessFactory.cpp"
  "${EMULATION_PSG_ROOT}/Emulation/Psg/Layer.cpp"
  "${EMULATION_PSG_ROOT}/Emulation/Psg/Executor/Component.cpp"
  "${EMULATION_PSG_ROOT}/Emulation/Psg/Executor/Node.cpp")
if(COMMAND score_register_license)
  score_register_license(emu2149
    URL "https://github.com/digital-sound-antiques/emu2149"
    NOTICES "${EMULATION_EMU2149_DIR}/emu2149.c"
    FILES "${EMULATION_EMU2149_DIR}/LICENSE")
  score_register_license(Nuked_PSG
    URL "https://github.com/nukeykt/Nuked-PSG"
    NOTICES "${EMULATION_NUKED_PSG_DIR}/ympsg.c" "${EMULATION_NUKED_PSG_DIR}/ympsg.h"
    FILES "${EMULATION_NUKED_PSG_DIR}/LICENSE")
endif()
