# Produce a self-contained source ZIP, excluding upstream bank collections and
# unused emulator cores. Run: cmake -P cmake/Release.cmake
cmake_minimum_required(VERSION 3.24)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(stage "${root}/release/score-addon-emulation")
if(EXISTS "${stage}")
  message(FATAL_ERROR "Release staging already exists: ${stage}. Move or remove it before packaging.")
endif()
file(MAKE_DIRECTORY "${stage}")
foreach(path IN ITEMS CMakeLists.txt addon.json score_addon_emulation.cpp
    score_addon_emulation.hpp LICENSE README.md DEPENDENCIES.md CHANGELOG.md
    Emulation Tests Tools cmake)
  file(COPY "${root}/${path}" DESTINATION "${stage}")
endforeach()
set(dep "${root}/3rdparty/libADLMIDI")
set(dest "${stage}/3rdparty/libADLMIDI")
file(MAKE_DIRECTORY "${dest}/src/chips")
file(COPY "${dep}/LICENSE.txt" "${dep}/LICENSE.LGPL-2.1.txt"
    "${dep}/include" DESTINATION "${dest}")
file(GLOB headers "${dep}/src/*.h" "${dep}/src/*.hpp" "${dep}/src/*.tcc")
file(COPY ${headers} DESTINATION "${dest}/src")
foreach(source IN ITEMS adlmidi.cpp adlmidi_load.cpp adlmidi_midiplay.cpp
    adlmidi_opl3.cpp adlmidi_private.cpp)
  file(COPY "${dep}/src/${source}" DESTINATION "${dest}/src")
endforeach()
foreach(path IN ITEMS models structures wopl)
  file(COPY "${dep}/src/${path}" DESTINATION "${dest}/src")
endforeach()
file(COPY "${dep}/src/midiseq/file_reader.hpp"
    "${dep}/src/midiseq/LICENSE.txt" DESTINATION "${dest}/src/midiseq")
file(COPY "${dep}/src/midiseq/impl/dpmi_alloc.hpp"
    DESTINATION "${dest}/src/midiseq/impl")
foreach(path IN ITEMS LICENSE.txt opl_chip_base.h opl_chip_base.tcc
    ymfm_opl2.cpp ymfm_opl2.h ymfm_opl3.cpp ymfm_opl3.h ymfm common)
  file(COPY "${dep}/src/chips/${path}" DESTINATION "${dest}/src/chips")
endforeach()
foreach(dependency IN ITEMS emu2149 Nuked-PSG)
  set(dep "${root}/3rdparty/${dependency}")
  set(dest "${stage}/3rdparty/${dependency}")
  if(dependency STREQUAL "emu2149")
    file(COPY "${dep}/emu2149.c" "${dep}/emu2149.h" "${dep}/LICENSE" DESTINATION "${dest}")
  else()
    file(COPY "${dep}/ympsg.c" "${dep}/ympsg.h" "${dep}/LICENSE" DESTINATION "${dest}")
  endif()
endforeach()
set(dep "${root}/3rdparty/libresidfp")
set(dest "${stage}/3rdparty/libresidfp")
file(COPY "${dep}/COPYING" DESTINATION "${dest}")
file(COPY "${dep}/src/" DESTINATION "${dest}/src"
  FILES_MATCHING PATTERN "*.h" PATTERN "*.cpp" PATTERN "siddefs-fp.h.in"
  PATTERN "residfp" EXCLUDE PATTERN "State.cpp" EXCLUDE PATTERN "test.cpp" EXCLUDE)
file(COPY "${dep}/src/residfp/residfp_defs.h" DESTINATION "${dest}/src/residfp")
file(COPY "${root}/3rdparty/munt/mt32emu" DESTINATION "${stage}/3rdparty/munt"
  PATTERN ".git*" EXCLUDE PATTERN "test" EXCLUDE)
set(dep "${root}/3rdparty/retromulator")
set(dest "${stage}/3rdparty/retromulator")
file(COPY "${dep}/LICENSE.txt" DESTINATION "${dest}")
file(GLOB headers "${dep}/source/openWurliLib/ow*.h")
file(COPY ${headers} DESTINATION "${dest}/source/openWurliLib")
file(COPY "${root}/3rdparty/emu2212/emu2212.c" "${root}/3rdparty/emu2212/emu2212.h"
  "${root}/3rdparty/emu2212/LICENSE" DESTINATION "${stage}/3rdparty/emu2212")
file(COPY "${root}/3rdparty/libvgm/emu/cores/c6280_mame.c"
  DESTINATION "${stage}/3rdparty/libvgm/emu/cores")
set(dep "${root}/3rdparty/apu/game-music-emu")
set(dest "${stage}/3rdparty/apu/game-music-emu")
file(COPY "${dep}/license.txt" DESTINATION "${dest}")
foreach(source IN ITEMS Nes_Apu.cpp Nes_Apu.h Nes_Oscs.cpp Nes_Oscs.h Blip_Buffer.cpp
    Blip_Buffer.h blargg_common.h blargg_config.h blargg_source.h)
  file(COPY "${dep}/gme/${source}" DESTINATION "${dest}/gme")
endforeach()
set(dep "${root}/3rdparty/apu/gb_apu")
set(dest "${stage}/3rdparty/apu/gb_apu")
file(COPY "${dep}/LICENCE.txt" "${dep}/gb_apu.c" "${dep}/gb_apu.h" DESTINATION "${dest}")
foreach(source IN ITEMS blip_buf.c blip_buf.h blip_wrap.c blip_wrap.h)
  file(COPY "${dep}/blargg/${source}" DESTINATION "${dest}/blargg")
endforeach()
file(COPY "${root}/3rdparty/apu/sources.json" DESTINATION "${stage}/3rdparty/apu")
file(COPY "${root}/3rdparty/notices" DESTINATION "${stage}/3rdparty")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf
    "${root}/score-addon-emulation.zip" --format=zip score-addon-emulation
    WORKING_DIRECTORY "${root}/release" COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Created ${root}/score-addon-emulation.zip")
