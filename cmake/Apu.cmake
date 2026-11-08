# SPDX-License-Identifier: GPL-3.0-or-later
# Submodules on the jcelerier forks' ossia-2026-11 branches:
# 3rdparty/apu/game-music-emu: libgme/game-music-emu f68963b1de0633149b05732113d3c3113c6f63a1
#   plus two Nes_Apu inspection accessors (LGPL-2.1-or-later).
# 3rdparty/apu/gb_apu: ITotalJustice/gb_apu 3d73d0df027a82d854cacd72a179c2d6a1a9703e
#   plus filter, routing, timing and UB fixes (MIT).
# Only the chip cores are built: no music player, CPU, ROM loader, game data or frontends.
# Upstream blobs and behaviour notes: 3rdparty/apu/sources.json.
get_filename_component(EMULATION_APU_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EMULATION_NES_APU_DIR "${EMULATION_APU_ROOT}/3rdparty/apu/game-music-emu")
set(EMULATION_GB_APU_DIR "${EMULATION_APU_ROOT}/3rdparty/apu/gb_apu")
if(NOT EXISTS "${EMULATION_NES_APU_DIR}/gme/Nes_Apu.cpp" OR
    NOT EXISTS "${EMULATION_GB_APU_DIR}/gb_apu.c")
  message(FATAL_ERROR "Emulation APU requires the 3rdparty/apu/game-music-emu and 3rdparty/apu/gb_apu submodules. Run git submodule update --init --recursive.")
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
    URL "https://github.com/jcelerier/game-music-emu/tree/ossia-2026-11"
    HEADER "LGPL-2.1-or-later"
    NOTICES "${EMULATION_NES_APU_DIR}/gme/Nes_Apu.cpp"
      "${EMULATION_NES_APU_DIR}/gme/Nes_Oscs.cpp"
      "${EMULATION_NES_APU_DIR}/gme/Blip_Buffer.cpp"
    FILES "${EMULATION_NES_APU_DIR}/license.txt")
  score_register_license(gb_apu
    URL "https://github.com/jcelerier/gb_apu/tree/ossia-2026-11"
    HEADER "MIT; blip_buf: MIT"
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
