cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

option(EMULATION_CORE_ONLY "Build headless emulation cores and tools without score" OFF)
if(NOT EMULATION_CORE_ONLY AND NOT TARGET score_lib_base)
  include(ScoreExternalAddon)
endif()
if(NOT EMULATION_CORE_ONLY AND
    (NOT TARGET score_plugin_engine OR NOT TARGET score_plugin_media))
  message(STATUS "Emulation add-on disabled: score's engine and media plug-ins are required.")
  return()
endif()

project(score_addon_emulation LANGUAGES C CXX)
if(NOT EMULATION_CORE_ONLY)
  score_common_setup()
endif()

include(cmake/AdlMidi.cmake)
include(cmake/Psg.cmake)
include(cmake/Sid.cmake)
include(cmake/La.cmake)
include(cmake/Wurli.cmake)
include(cmake/Wavetable.cmake)
include(cmake/Apu.cmake)
add_library(score_emulation_opl STATIC Emulation/Opl/Engine.cpp)
target_compile_features(score_emulation_opl PUBLIC cxx_std_20)
target_include_directories(score_emulation_opl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(score_emulation_opl PRIVATE
  "${EMULATION_ADLMIDI_DIR}/src" "${EMULATION_ADLMIDI_DIR}/src/wopl")
target_link_libraries(score_emulation_opl PUBLIC score_emulation_adlmidi)
set_target_properties(score_emulation_opl PROPERTIES
  POSITION_INDEPENDENT_CODE ON UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)

option(EMULATION_BUILD_TOOLS "Build the offline OPL WAV renderer" ${EMULATION_CORE_ONLY})
if(EMULATION_BUILD_TOOLS)
  add_executable(emulation-opl-render Tools/OplRender.cpp)
  target_link_libraries(emulation-opl-render PRIVATE score_emulation_opl)
endif()

if(NOT EMULATION_CORE_ONLY)
  set(EMULATION_MODEL_SOURCES
    Emulation/Opl/Controls.cpp
    Emulation/Opl/ProcessModel.cpp
    Emulation/Opl/ProcessModelSerialization.cpp
    Emulation/Opl/ProcessFactory.cpp
    Emulation/Opl/Layer.cpp)
  add_library(${PROJECT_NAME}
    ${EMULATION_MODEL_SOURCES}
    ${EMULATION_PSG_NATIVE_SOURCES}
    ${EMULATION_SID_NATIVE_SOURCES}
    ${EMULATION_LA_NATIVE_SOURCES}
    ${EMULATION_WURLI_NATIVE_SOURCES}
    ${EMULATION_WAVETABLE_NATIVE_SOURCES}
    ${EMULATION_APU_NATIVE_SOURCES}
    Emulation/Opl/Executor/Component.cpp
    Emulation/Opl/Executor/Node.cpp
    score_addon_emulation.cpp
    score_addon_emulation.hpp)
  target_link_libraries(${PROJECT_NAME}
    PUBLIC score_lib_base score_plugin_engine score_plugin_media score_plugin_dataflow
    PRIVATE score_plugin_scenario score_emulation_opl score_emulation_psg score_emulation_sid
            score_emulation_la score_emulation_wurli
            score_emulation_wavetable score_emulation_apu)
  setup_score_plugin(${PROJECT_NAME})
endif()

option(EMULATION_BUILD_TESTS "Build emulation behavior tests" ${SCORE_TESTING})
if(EMULATION_BUILD_TESTS)
  enable_testing()
  if(COMMAND score_setup_catch2)
    score_setup_catch2()
  elseif(NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 REQUIRED)
  endif()
  foreach(family IN ITEMS Opl Psg Sid La Wurli Wavetable Apu)
    string(TOLOWER "${family}" family_lower)
    add_executable(Emulation_${family}EngineTest Tests/${family}EngineTest.cpp)
    target_link_libraries(Emulation_${family}EngineTest PRIVATE
      score_emulation_${family_lower} Catch2::Catch2WithMain)
    set_target_properties(Emulation_${family}EngineTest PROPERTIES
      UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
    add_test(NAME Emulation_${family}EngineTest COMMAND Emulation_${family}EngineTest)

    if(NOT EMULATION_CORE_ONLY AND COMMAND score_add_test)
      set(emulation_test_extra_plugins)
      if(family STREQUAL "Wavetable" AND TARGET score_plugin_js)
        list(APPEND emulation_test_extra_plugins score_plugin_js)
      endif()
      score_add_test(Emulation_${family}DocumentTest
        SOURCES Tests/${family}DocumentTest.cpp
        APP
        PLUGINS score_plugin_engine score_plugin_scenario score_lib_process score_plugin_dataflow score_plugin_curve ${emulation_test_extra_plugins}
        LIBS score_emulation_${family_lower} score_addon_emulation)
      if(family STREQUAL "Wavetable")
        target_sources(Emulation_WavetableDocumentTest PRIVATE Tests/WavetablePresetTest.cpp)
      endif()
      target_include_directories(Emulation_${family}DocumentTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
        "${SCORE_ROOT_SOURCE_DIR}/tests/fixtures")
    endif()
  endforeach()
  if(NOT EMULATION_CORE_ONLY AND COMMAND score_add_test)
    score_add_test(Emulation_ChipQualificationTest
      SOURCES Tests/ChipQualificationTest.cpp APP
      PLUGINS score_plugin_engine score_plugin_scenario score_lib_process
      LIBS score_emulation_opl score_emulation_psg score_emulation_sid score_emulation_wurli score_addon_emulation)
    target_include_directories(Emulation_ChipQualificationTest PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
      "${SCORE_ROOT_SOURCE_DIR}/tests/fixtures")
  endif()
endif()
