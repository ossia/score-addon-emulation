# SPDX-License-Identifier: GPL-3.0-or-later
# Explicit source selection keeps upstream's global flags/options and bundled
# banks out of score. The submodule itself is intentionally unmodified.
set(EMULATION_ADLMIDI_DIR "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libADLMIDI")
if(NOT EXISTS "${EMULATION_ADLMIDI_DIR}/include/adlmidi.h")
  message(FATAL_ERROR "Emulation requires its pinned libADLMIDI submodule. Run git submodule update --init --recursive in score-addon-emulation.")
endif()

include("${EMULATION_ADLMIDI_DIR}/src/models/opl_models.cmake")
add_library(score_emulation_adlmidi STATIC
  "${EMULATION_ADLMIDI_DIR}/src/adlmidi.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/adlmidi_load.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/adlmidi_midiplay.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/adlmidi_opl3.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/adlmidi_private.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/wopl/wopl_file.c"
  ${OPL_MODELS_SOURCES}
  "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm_opl2.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm_opl3.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm/ymfm_adpcm.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm/ymfm_misc.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm/ymfm_opl.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm/ymfm_pcm.cpp"
  "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm/ymfm_ssg.cpp"
)
target_include_directories(score_emulation_adlmidi PUBLIC
  "${EMULATION_ADLMIDI_DIR}/include")
target_compile_definitions(score_emulation_adlmidi PUBLIC
  DISABLE_EMBEDDED_BANKS
  ADLMIDI_DISABLE_MIDI_SEQUENCER
  BWMIDI_DISABLE_XMI_SUPPORT
  ADLMIDI_DISABLE_DOSBOX_EMULATOR
  ADLMIDI_DISABLE_NUKED_EMULATOR
  ADLMIDI_DISABLE_OPAL_EMULATOR
  ADLMIDI_DISABLE_JAVA_EMULATOR
  ADLMIDI_DISABLE_ESFMU_EMULATOR
  ADLMIDI_DISABLE_MAME_OPL2_EMULATOR
)
set_target_properties(score_emulation_adlmidi PROPERTIES
  C_STANDARD 99 CXX_STANDARD 14
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
if(MSVC)
  target_compile_definitions(score_emulation_adlmidi PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(UNIX)
  target_link_libraries(score_emulation_adlmidi PRIVATE m)
endif()

if(COMMAND score_register_license)
  score_register_license(libADLMIDI
    URL "https://github.com/Wohlstand/libADLMIDI"
    NOTICES "${EMULATION_ADLMIDI_DIR}/src/adlmidi.cpp"
    FILES "${EMULATION_ADLMIDI_DIR}/LICENSE.txt")
  score_register_license(libADLMIDI_chip_interfaces
    URL "https://github.com/Wohlstand/libADLMIDI"
    NOTICES "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm_opl3.cpp"
    FILES "${EMULATION_ADLMIDI_DIR}/LICENSE.LGPL-2.1.txt")
  score_register_license(ymfm
    URL "https://github.com/aaronsgiles/ymfm"
    FILES "${EMULATION_ADLMIDI_DIR}/src/chips/ymfm/LICENSE")
  score_register_license(wopl
    URL "https://github.com/Wohlstand/libADLMIDI"
    FILES "${EMULATION_ADLMIDI_DIR}/src/wopl/LICENSE.txt")
  score_register_license(opl_volume_models
    URL "https://github.com/Wohlstand/libADLMIDI"
    FILES "${EMULATION_ADLMIDI_DIR}/src/models/LICENSE.txt")
  score_register_license(libADLMIDI_fixed_storage
    URL "https://github.com/Wohlstand/libADLMIDI"
    FILES "${EMULATION_ADLMIDI_DIR}/src/structures/LICENSE")
  score_register_license(libADLMIDI_file_and_allocator_helpers
    URL "https://github.com/Wohlstand/libADLMIDI"
    FILES "${EMULATION_ADLMIDI_DIR}/src/midiseq/LICENSE.txt")
endif()
