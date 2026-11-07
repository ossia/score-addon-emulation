# SPDX-License-Identifier: GPL-3.0-or-later
# Curated source-only pins; full provenance and licenses live beside each core.
# emu2212 e1735c83707e6ee739a123476344659f7fa9fb18 (MIT)
# libvgm a6eed945ecf4e5a25cf82004779c6f7e4fd5c319 c6280_mame.c (BSD-3-Clause)
set(EMULATION_WAVETABLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(score_emulation_wavetable STATIC
  "${EMULATION_WAVETABLE_ROOT}/Emulation/Wavetable/Engine.cpp"
  "${EMULATION_WAVETABLE_ROOT}/3rdparty/emu2212/emu2212.c"
  "${EMULATION_WAVETABLE_ROOT}/3rdparty/huc6280/adapter.c")
target_compile_features(score_emulation_wavetable PUBLIC cxx_std_20)
target_include_directories(score_emulation_wavetable PUBLIC "${EMULATION_WAVETABLE_ROOT}"
  PRIVATE "${EMULATION_WAVETABLE_ROOT}/3rdparty/emu2212")
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
  score_register_license(emu2212 URL "https://github.com/digital-sound-antiques/emu2212"
    FILES "${EMULATION_WAVETABLE_ROOT}/3rdparty/emu2212/LICENSE")
  score_register_license(HuC6280_MAME URL "https://github.com/ValleyBell/libvgm"
    NOTICES "${EMULATION_WAVETABLE_ROOT}/3rdparty/huc6280/emu/cores/c6280_mame.c"
    FILES "${EMULATION_WAVETABLE_ROOT}/3rdparty/huc6280/LICENSE")
endif()
