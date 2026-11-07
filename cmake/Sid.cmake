# SPDX-License-Identifier: GPL-3.0-or-later
# libresidfp fa9dd25d16863493b36b6459128d6583fb3d179f, GPL-2.0-or-later.
# Select the chip core and scalar resampler, not autotools, players, tests or assets.
get_filename_component(EMULATION_SID_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EMULATION_RESIDFP_DIR "${EMULATION_SID_ROOT}/3rdparty/libresidfp")
if(NOT EXISTS "${EMULATION_RESIDFP_DIR}/src/SID.cpp")
  message(FATAL_ERROR "Emulation SID requires its pinned 3rdparty/libresidfp submodule.")
endif()
function(emulation_configure_residfp)
  set(RESIDFP_BRANCH_HINTS 0)
  set(HAVE_BUILTIN_EXPECT 0)
  set(RESIDFP_INLINING 1)
  set(RESIDFP_INLINE inline)
  set(PACKAGE_VERSION "1.3-dev-fa9dd25")
  configure_file("${EMULATION_RESIDFP_DIR}/src/siddefs-fp.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/emulation-residfp/siddefs-fp.h" @ONLY)
endfunction()
emulation_configure_residfp()
find_package(Threads REQUIRED)
add_library(score_emulation_sid STATIC
  "${EMULATION_SID_ROOT}/Emulation/Sid/Engine.cpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Patch.cpp"
  "${EMULATION_RESIDFP_DIR}/src/Dac.cpp"
  "${EMULATION_RESIDFP_DIR}/src/EnvelopeGenerator.cpp"
  "${EMULATION_RESIDFP_DIR}/src/ExternalFilter.cpp"
  "${EMULATION_RESIDFP_DIR}/src/Filter.cpp"
  "${EMULATION_RESIDFP_DIR}/src/Filter6581.cpp"
  "${EMULATION_RESIDFP_DIR}/src/Filter8580.cpp"
  "${EMULATION_RESIDFP_DIR}/src/FilterModelConfig.cpp"
  "${EMULATION_RESIDFP_DIR}/src/FilterModelConfig6581.cpp"
  "${EMULATION_RESIDFP_DIR}/src/FilterModelConfig8580.cpp"
  "${EMULATION_RESIDFP_DIR}/src/Integrator6581.cpp"
  "${EMULATION_RESIDFP_DIR}/src/Integrator8580.cpp"
  "${EMULATION_RESIDFP_DIR}/src/OpAmp.cpp"
  "${EMULATION_RESIDFP_DIR}/src/SID.cpp"
  "${EMULATION_RESIDFP_DIR}/src/Spline.cpp"
  "${EMULATION_RESIDFP_DIR}/src/WaveformCalculator.cpp"
  "${EMULATION_RESIDFP_DIR}/src/WaveformGenerator.cpp"
  "${EMULATION_RESIDFP_DIR}/src/resample/SincResampler.cpp")
target_include_directories(score_emulation_sid PUBLIC "${EMULATION_SID_ROOT}"
  PRIVATE "${EMULATION_RESIDFP_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/emulation-residfp")
target_compile_features(score_emulation_sid PUBLIC cxx_std_20)
target_link_libraries(score_emulation_sid PRIVATE Threads::Threads)
set_target_properties(score_emulation_sid PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  UNITY_BUILD OFF DISABLE_PRECOMPILE_HEADERS ON)
if(MSVC)
  target_compile_options(score_emulation_sid PRIVATE /Zc:__cplusplus)
endif()
if(UNIX)
  target_link_libraries(score_emulation_sid PRIVATE m)
endif()
if(COMMAND score_register_license)
  score_register_license(libresidfp
    URL "https://github.com/libsidplayfp/libresidfp"
    NOTICES "${EMULATION_RESIDFP_DIR}/src/SID.cpp"
    FILES "${EMULATION_RESIDFP_DIR}/COPYING")
endif()
set(EMULATION_SID_NATIVE_SOURCES
  "${EMULATION_SID_ROOT}/Emulation/Sid/Controls.hpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Controls.cpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/ProcessMetadata.hpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/ProcessModel.hpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/ProcessModel.cpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/ProcessModelSerialization.cpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/ProcessFactory.hpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/ProcessFactory.cpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Layer.hpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Layer.cpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Executor/Node.hpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Executor/Node.cpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Executor/Component.hpp"
  "${EMULATION_SID_ROOT}/Emulation/Sid/Executor/Component.cpp")
