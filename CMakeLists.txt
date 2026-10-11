cmake_minimum_required(VERSION 3.21)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Version minimale de macOS")
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Mac Intel et Apple Silicon" FORCE)
endif()
# Windows : runtime C++ intégré au plugin (pas besoin d'installer le redistribuable Visual C++)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(raw-synth VERSION 0.3.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

# Sans type de build, Makefiles et Ninja compilent sans optimisation : on force Release
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Type de build" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(RAW_CLAP_ONLY "Construire seulement le .clap, sans VST3 ni AU" OFF)
set(RAW_CLAP_WRAPPER_TAG "main" CACHE STRING "Version de clap-wrapper (mettre une version publiée, ex. celle de la page Releases)")
set(RAW_WEBVIEW2_VERSION "1.0.2210.55" CACHE STRING "Version du SDK WebView2 (paquet NuGet Microsoft.Web.WebView2)")

include(FetchContent)

# En-têtes officiels CLAP (licence MIT)
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(clap)

# Page de l'interface intégrée dans le plugin
set(RAW_GEN ${CMAKE_BINARY_DIR}/gen)
add_custom_command(
    OUTPUT ${RAW_GEN}/ui_html.h
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/ui/raw_ui.html -DOUT=${RAW_GEN}/ui_html.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/raw_ui.html ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Integration de l'interface RAW"
    VERBATIM)

# Le synthé lui-même
add_library(raw-impl STATIC src/plugin.cpp ${RAW_GEN}/ui_html.h)
target_include_directories(raw-impl PUBLIC src ${RAW_GEN})
target_link_libraries(raw-impl PUBLIC clap)
if(MSVC)
    target_compile_options(raw-impl PRIVATE /W3 /utf-8)
else()
    target_compile_options(raw-impl PRIVATE -Wall -Wextra)
endif()

# Fenêtre d'interface selon le système
if(WIN32)
    FetchContent_Declare(webview2
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${RAW_WEBVIEW2_VERSION}
        DOWNLOAD_NAME webview2.zip)
    FetchContent_MakeAvailable(webview2)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64")
        set(RAW_WV2_ARCH arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(RAW_WV2_ARCH x64)
    else()
        set(RAW_WV2_ARCH x86)
    endif()
    target_sources(raw-impl PRIVATE src/gui_win.cpp)
    target_include_directories(raw-impl PRIVATE ${webview2_SOURCE_DIR}/build/native/include)
    target_link_libraries(raw-impl PUBLIC
        ${webview2_SOURCE_DIR}/build/native/${RAW_WV2_ARCH}/WebView2LoaderStatic.lib
        ole32 oleaut32 user32 version)
elseif(APPLE)
    target_sources(raw-impl PRIVATE src/gui_mac.mm)
    set_source_files_properties(src/gui_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(raw-impl PUBLIC "-framework Cocoa" "-framework WebKit")
else()
    target_sources(raw-impl PRIVATE src/gui_stub.cpp)
endif()

if(RAW_CLAP_ONLY)
    # Plan B : uniquement le format CLAP, sans aucun outil de conversion
    add_library(RAW MODULE src/entry.cpp)
    target_link_libraries(RAW PRIVATE raw-impl)
    if(APPLE)
        set_target_properties(RAW PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION clap
            MACOSX_BUNDLE_GUI_IDENTIFIER com.raw.synth)
    else()
        set_target_properties(RAW PROPERTIES PREFIX "" SUFFIX ".clap")
    endif()
else()
    # clap-wrapper (projet officiel CLAP, licence MIT) fabrique le VST3 et l'AU à partir
    # du même code, interface comprise. Il télécharge lui-même le SDK VST3 de Steinberg.
    # Les options de make_clapfirst_plugins sont à comparer avec le README de la version choisie.
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
    FetchContent_Declare(clap-wrapper
        GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
        GIT_TAG ${RAW_CLAP_WRAPPER_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(clap-wrapper)

    make_clapfirst_plugins(
        TARGET_NAME raw
        IMPL_TARGET raw-impl
        OUTPUT_NAME "RAW"
        ENTRY_SOURCE src/entry.cpp
        BUNDLE_IDENTIFIER "com.raw.synth"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS CLAP VST3 AUV2
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
        AUV2_MANUFACTURER_NAME "RAW"
        AUV2_MANUFACTURER_CODE "RaWs"
        AUV2_SUBTYPE_CODE "Raw1"
        AUV2_INSTRUMENT_TYPE "aumu"
    )
endif()
