# Axell core: shared DSP (JUCE-free headers) and the Axell look (JUCE GUI sources) for every Axell plugin.
# Use from a plugin project after JUCE is available:
#   add_subdirectory(<path to axell-core> axell-core)
#   target_link_libraries(MyPlugin PRIVATE axell_core_dsp)
#   axell_core_add_gui(MyPlugin)          # Axell look and feel + Inter fonts compiled into MyPlugin
cmake_minimum_required(VERSION 3.22)

add_library(axell_core_dsp INTERFACE)
target_include_directories(axell_core_dsp INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(axell_core_dsp INTERFACE cxx_std_17)

set(AXELL_CORE_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE INTERNAL "")
if(COMMAND juce_add_binary_data AND NOT TARGET AxellCoreFonts)
  juce_add_binary_data(AxellCoreFonts HEADER_NAME AxellCoreFonts.h NAMESPACE AxellFonts SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/resources/fonts/Inter-Regular.otf
    ${CMAKE_CURRENT_LIST_DIR}/resources/fonts/Inter-SemiBold.otf
    ${CMAKE_CURRENT_LIST_DIR}/resources/fonts/Inter-Bold.otf)
  set_target_properties(AxellCoreFonts PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# GUI sources are compiled into each plugin target (JUCE modules are per target)
function(axell_core_add_gui target)
  target_sources(${target} PRIVATE ${AXELL_CORE_DIR}/src/gui/LookAndFeel.cpp)
  target_include_directories(${target} PRIVATE ${AXELL_CORE_DIR}/include)
  target_link_libraries(${target} PRIVATE AxellCoreFonts axell_core_dsp)
endfunction()

option(AXELL_CORE_TESTS "Build the Axell core DSP tests" ON)
if(AXELL_CORE_TESTS)
  add_executable(AxellCoreTests ${CMAKE_CURRENT_LIST_DIR}/tests/CoreTests.cpp)
  target_link_libraries(AxellCoreTests PRIVATE axell_core_dsp)
  target_compile_options(AxellCoreTests PRIVATE -Wall -Wextra -Wshadow $<$<CONFIG:Release>:-O3>)
endif()
