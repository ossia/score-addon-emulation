# SPDX-License-Identifier: GPL-3.0-or-later
# munt/munt 6e7c01fba7e1d50c8fa705834889fd0eac136075, mt32emu 2.8.3.
# Local extension: timestamped Synth::playMsgOnPart uses the native short-message
# queue and renderer's abort/retry handling without changing part assignments.
# Queued coalesced-key release ignores pedal only for the retriggered melodic
# key; part CC120 exposes native allSoundOff decay without a global reset.
# Headless C++ engine + built-in resampler only; no frontends, ROMs or fake-ROM tests.
get_filename_component(EMULATION_LA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EMULATION_MUNT_DIR "${EMULATION_LA_ROOT}/3rdparty/munt/mt32emu")
if(NOT EXISTS "${EMULATION_MUNT_DIR}/src/Synth.cpp")
  message(FATAL_ERROR "Emulation LA requires the pinned 3rdparty/munt/mt32emu sources.")
endif()
function(emulation_configure_munt)
  include("${EMULATION_MUNT_DIR}/cmake/project_data.cmake")
  set(libmt32emu_EXPORTS_TYPE 0)
  set(libmt32emu_SHARED_DEFINITION "#undef MT32EMU_SHARED")
  set(libmt32emu_RUNTIME_VERSION_CHECK 0)
  configure_file("${EMULATION_MUNT_DIR}/src/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/emulation-munt/include/mt32emu/config.h" @ONLY)
  foreach(header mt32emu.h globals.h Enumerations.h Types.h File.h FileStream.h
      MidiStreamParser.h ROMInfo.h SampleRateConverter.h Synth.h)
    configure_file("${EMULATION_MUNT_DIR}/src/${header}"
      "${CMAKE_CURRENT_BINARY_DIR}/emulation-munt/include/mt32emu/${header}" COPYONLY)
  endforeach()
endfunction()
emulation_configure_munt()
set(EMULATION_MUNT_SOURCES)
foreach(source Analog BReverbModel Display File FileStream LA32FloatWaveGenerator LA32Ramp
    LA32WaveGenerator MidiStreamParser Part Partial PartialManager Poly ROMInfo Synth
    Tables TVA TVF TVP SampleRateConverter sha1/sha1 srchelper/InternalResampler
    srchelper/srctools/src/FIRResampler srchelper/srctools/src/SincResampler
    srchelper/srctools/src/IIR2xResampler srchelper/srctools/src/LinearResampler
    srchelper/srctools/src/ResamplerModel)
  list(APPEND EMULATION_MUNT_SOURCES "${EMULATION_MUNT_DIR}/src/${source}.cpp")
endforeach()
add_library(score_emulation_la STATIC
  "${EMULATION_LA_ROOT}/Emulation/La/Engine.cpp"
  "${EMULATION_LA_ROOT}/Emulation/La/Patch.cpp"
  "${EMULATION_LA_ROOT}/Emulation/La/VoicePolicy.hpp"
  ${EMULATION_MUNT_SOURCES})
target_include_directories(score_emulation_la PUBLIC "${EMULATION_LA_ROOT}"
  PRIVATE "${EMULATION_MUNT_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/emulation-munt/include"
    "${CMAKE_CURRENT_BINARY_DIR}/emulation-munt/include/mt32emu")
target_compile_features(score_emulation_la PUBLIC cxx_std_20)
target_compile_definitions(score_emulation_la PRIVATE MT32EMU_WITH_INTERNAL_RESAMPLER MT32EMU_WITH_STD_SNPRINTF)
set_target_properties(score_emulation_la PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
if(UNIX)
  target_link_libraries(score_emulation_la PRIVATE m)
endif()
if(COMMAND score_register_license)
  score_register_license(munt
    URL "https://github.com/munt/munt/tree/6e7c01fba7e1d50c8fa705834889fd0eac136075/mt32emu"
    NOTICES "${EMULATION_MUNT_DIR}/src/sha1/sha1.h"
    FILES "${EMULATION_MUNT_DIR}/AUTHORS.txt" "${EMULATION_MUNT_DIR}/COPYING.txt" "${EMULATION_MUNT_DIR}/COPYING.LESSER.txt")
endif()
set(EMULATION_LA_NATIVE_SOURCES)
foreach(source Controls.hpp Controls.cpp ProcessMetadata.hpp ProcessModel.hpp ProcessModel.cpp
    ProcessModelSerialization.cpp ProcessFactory.hpp ProcessFactory.cpp Layer.hpp Layer.cpp
    Executor/Node.hpp Executor/Node.cpp Executor/Component.hpp Executor/Component.cpp)
  list(APPEND EMULATION_LA_NATIVE_SOURCES "${EMULATION_LA_ROOT}/Emulation/La/${source}")
endforeach()
