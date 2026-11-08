# SPDX-License-Identifier: GPL-3.0-or-later
# Submodules on the jcelerier forks' ossia-2026-11 branches:
# 3rdparty/emu2212: digital-sound-antiques/emu2212 e1735c83707e6ee739a123476344659f7fa9fb18
#   plus the update_output rounding fix (MIT).
# 3rdparty/libvgm: ValleyBell/libvgm a6eed945ecf4e5a25cf82004779c6f7e4fd5c319 plus a
#   standalone, UB-free, 32-bit-phase emu/cores/c6280_mame.c (BSD-3-Clause), the
#   only libvgm file built, through Emulation/Wavetable/HucCore.c.
set(EMULATION_WAVETABLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(EMULATION_EMU2212_DIR "${EMULATION_WAVETABLE_ROOT}/3rdparty/emu2212")
set(EMULATION_LIBVGM_DIR "${EMULATION_WAVETABLE_ROOT}/3rdparty/libvgm")
if(NOT EXISTS "${EMULATION_EMU2212_DIR}/emu2212.c" OR
    NOT EXISTS "${EMULATION_LIBVGM_DIR}/emu/cores/c6280_mame.c")
  message(FATAL_ERROR "Emulation Wavetable requires the 3rdparty/emu2212 and 3rdparty/libvgm submodules. Run git submodule update --init --recursive.")
endif()
add_library(score_emulation_wavetable STATIC
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/Engine.cpp"
  "${EMULATION_EMU2212_DIR}/emu2212.c"
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/HucCore.c")
# Scoped to the adapter: the libvgm root also holds framework headers.
set_source_files_properties("${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/HucCore.c"
  PROPERTIES INCLUDE_DIRECTORIES "${EMULATION_LIBVGM_DIR}")
target_compile_features(score_emulation_wavetable PUBLIC cxx_std_20)
target_include_directories(score_emulation_wavetable PUBLIC "${EMULATION_WAVETABLE_ROOT}"
  PRIVATE "${EMULATION_EMU2212_DIR}")
set_target_properties(score_emulation_wavetable PROPERTIES
  C_STANDARD 99 POSITION_INDEPENDENT_CODE ON UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
if(UNIX)
  target_link_libraries(score_emulation_wavetable PRIVATE m)
endif()
set(EMULATION_WAVETABLE_NATIVE_SOURCES
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/Controls.cpp"
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/ProcessModel.cpp"
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/ProcessModelSerialization.cpp"
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/ProcessFactory.cpp"
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/Layer.cpp"
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/Executor/Component.cpp"
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/Executor/Node.cpp")
if(COMMAND score_register_license)
  score_register_license(emu2212
    URL "https://github.com/jcelerier/emu2212/tree/ossia-2026-11"
    HEADER "MIT"
    FILES "${EMULATION_EMU2212_DIR}/LICENSE")
  score_register_license(HuC6280_MAME
    URL "https://github.com/jcelerier/libvgm/tree/ossia-2026-11/emu/cores"
    HEADER "BSD-3-Clause"
    NOTICES "${EMULATION_LIBVGM_DIR}/emu/cores/c6280_mame.c"
    FILES "${EMULATION_WAVETABLE_ROOT}/3rdparty/notices/HuC6280_MAME-LICENSE.txt")
endif()
