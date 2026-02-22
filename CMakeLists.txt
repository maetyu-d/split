cmake_minimum_required(VERSION 3.22)

project(SplitAudioProgramming VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(SPLIT_ENABLE_ABLETON_LINK "Enable Ableton Link integration in PluginHost (requires Link SDK headers/libs)" OFF)

# Set this to your JUCE checkout location.
# Example: cmake -DJUCE_DIR=/path/to/JUCE ..
if(NOT DEFINED JUCE_DIR)
  message(FATAL_ERROR "JUCE_DIR is not set. Pass -DJUCE_DIR=/path/to/JUCE")
endif()

if(SPLIT_ENABLE_ABLETON_LINK)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/link/AbletonLinkConfig.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/link/AbletonLinkConfig.cmake")
    message(STATUS "Ableton Link: enabled (vendored)")
  else()
    message(FATAL_ERROR "SPLIT_ENABLE_ABLETON_LINK=ON but third_party/link is missing. Clone Ableton Link into third_party/link.")
  endif()
endif()

add_subdirectory(${JUCE_DIR} JUCE)

juce_add_gui_app(PatternIDE
  PRODUCT_NAME "Pattern IDE"
)
juce_generate_juce_header(PatternIDE)

target_sources(PatternIDE PRIVATE
  Source/Common/OscProtocol.h
  Source/IDE/Main.cpp
  Source/IDE/MainWindow.h
  Source/IDE/MainWindow.cpp
)

target_link_libraries(PatternIDE PRIVATE
  juce::juce_audio_utils
  juce::juce_osc
  juce::juce_gui_extra
)

target_compile_definitions(PatternIDE PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
)

juce_add_gui_app(PluginHost
  PRODUCT_NAME "Plugin Host"
)
juce_generate_juce_header(PluginHost)

target_sources(PluginHost PRIVATE
  Source/Common/OscProtocol.h
  Source/Host/Main.cpp
  Source/Host/HostMainWindow.h
  Source/Host/HostMainWindow.cpp
  Source/Host/HostEngine.h
  Source/Host/HostEngine.cpp
)

target_link_libraries(PluginHost PRIVATE
  juce::juce_audio_processors
  juce::juce_audio_utils
  juce::juce_osc
  juce::juce_gui_extra
  $<$<BOOL:${SPLIT_ENABLE_ABLETON_LINK}>:Ableton::Link>
)

target_compile_definitions(PluginHost PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=1
  SPLIT_ENABLE_ABLETON_LINK=$<BOOL:${SPLIT_ENABLE_ABLETON_LINK}>
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
)
