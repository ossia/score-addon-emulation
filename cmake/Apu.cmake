# SPDX-License-Identifier: GPL-3.0-or-later
# libgme/game-music-emu f68963b1de0633149b05732113d3c3113c6f63a1 (LGPL-2.1-or-later).
# ITotalJustice/gb_apu 3d73d0df027a82d854cacd72a179c2d6a1a9703e (MIT).
# Source-only chip cores: no music player, CPU, ROM loader, game data or frontends.
# Exact upstream blobs and local correctness patches: 3rdparty/apu/sources.json.
get_filename_component(EMULATION_APU_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EMULATION_NES_APU_DIR "${EMULATION_APU_ROOT}/3rdparty/apu/game-music-emu")
set(EMULATION_GB_APU_DIR "${EMULATION_APU_ROOT}/3rdparty/apu/gb_apu")
if(NOT EXISTS "${EMULATION_NES_APU_DIR}/gme/Nes_Apu.cpp" OR
    NOT EXISTS "${EMULATION_GB_APU_DIR}/gb_apu.c")
  message(FATAL_ERROR "Emulation APU requires the curated pinned 3rdparty/apu sources.")
endif()
add_library(score_emulation_apu STATIC
  "${EMULATION_APU_ROOT}/Emulation/Apu/Engine.cpp"
  "${EMULATION_NES_APU_DIR}/gme/Nes_Apu.cpp"
  "${EMULATION_NES_APU_DIR}/gme/Nes_Oscs.cpp"
  "${EMULATION_NES_APU_DIR}/gme/Blip_Buffer.cpp"
  "${EMULATION_GB_APU_DIR}/gb_apu.c"
  "${EMULATION_GB_APU_DIR}/blargg/blip_wrap.c"
  "${EMULATION_GB_APU_DIR}/blargg/blip_buf.c")
target_compile_features(score_emulation_apu PUBLIC cxx_std_20)
target_include_directories(score_emulation_apu PUBLIC "${EMULATION_APU_ROOT}"
  PRIVATE "${EMULATION_NES_APU_DIR}/gme" "${EMULATION_GB_APU_DIR}")
# The supported C backend avoids a second incompatible global Blip_Buffer class.
# Math support is required for the genuine DMG/CGB capacitor high-pass filters.
target_compile_definitions(score_emulation_apu PRIVATE GB_APU_CXX=0 GB_APU_HAS_MATH_H=1)
set_target_properties(score_emulation_apu PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
if(UNIX)
  target_link_libraries(score_emulation_apu PRIVATE m)
endif()
if(COMMAND score_register_license)
  score_register_license(game_music_emu_nes_apu
    URL "https://github.com/libgme/game-music-emu/tree/f68963b1de0633149b05732113d3c3113c6f63a1"
    NOTICES "${EMULATION_NES_APU_DIR}/gme/Nes_Apu.cpp"
      "${EMULATION_NES_APU_DIR}/gme/Nes_Oscs.cpp"
      "${EMULATION_NES_APU_DIR}/gme/Blip_Buffer.cpp"
    FILES "${EMULATION_NES_APU_DIR}/license.txt")
  score_register_license(gb_apu
    URL "https://github.com/ITotalJustice/gb_apu/tree/3d73d0df027a82d854cacd72a179c2d6a1a9703e"
    NOTICES "${EMULATION_GB_APU_DIR}/blargg/blip_buf.c"
    FILES "${EMULATION_GB_APU_DIR}/LICENCE.txt")
endif()
set(EMULATION_APU_NATIVE_SOURCES)
foreach(source Controls.hpp Controls.cpp ProcessMetadata.hpp ProcessModel.hpp ProcessModel.cpp
    ProcessModelSerialization.cpp ProcessFactory.hpp ProcessFactory.cpp Layer.hpp Layer.cpp
    Executor/Node.hpp Executor/Node.cpp Executor/Component.hpp Executor/Component.cpp)
  list(APPEND EMULATION_APU_NATIVE_SOURCES "${EMULATION_APU_ROOT}/Emulation/Apu/${source}")
endforeach()
# Root's shared family loop registers Emulation_ApuEngineTest against this core and,
# only in host builds, Emulation_ApuDocumentTest against the native add-on.
