cmake_minimum_required(VERSION 3.16)
project(DeezerClient VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent OpenGLWidgets)

# libprojectM for visualizations (manual setup like BASS)
# Download from: https://github.com/projectM-visualizer/projectm

# OpenSSL required for mobile API (initializeKeys, token decrypt, ARL→mobile session)
# Set OPENSSL_ROOT_DIR if not in a standard location (e.g. -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64")
# Linux: apt install libssl-dev / dnf install openssl-devel
# OpenSSL: when OPENSSL_ROOT_DIR is set, pre-fill hints for Windows installer layout
# (Shining Light etc. use lib/ or lib/VC/x64/...; FindOpenSSL only checks lib/ and lib64/)
if(OPENSSL_ROOT_DIR)
    set(OPENSSL_USE_STATIC_LIBS OFF)
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include" CACHE PATH "OpenSSL include directory" FORCE)
    set(_ssl_lib "")
    set(_crypto_lib "")
    foreach(_libdir lib lib/VC/x64/MT/Release lib/VC/x64/MD/Release lib/VC/x64/MT/Debug lib/VC/x64/MD/Debug lib64)
        if(EXISTS "${OPENSSL_ROOT_DIR}/${_libdir}")
            file(GLOB _ssl_cand   "${OPENSSL_ROOT_DIR}/${_libdir}/libssl*.lib"   "${OPENSSL_ROOT_DIR}/${_libdir}/ssl*.lib")
            file(GLOB _crypto_cand "${OPENSSL_ROOT_DIR}/${_libdir}/libcrypto*.lib" "${OPENSSL_ROOT_DIR}/${_libdir}/crypto*.lib")
            if(_ssl_cand AND _crypto_cand)
                list(GET _ssl_cand 0 _ssl_lib)
                list(GET _crypto_cand 0 _crypto_lib)
                set(OPENSSL_SSL_LIBRARY "${_ssl_lib}" CACHE FILEPATH "OpenSSL SSL library" FORCE)
                set(OPENSSL_CRYPTO_LIBRARY "${_crypto_lib}" CACHE FILEPATH "OpenSSL Crypto library" FORCE)
                break()
            endif()
        endif()
    endforeach()
endif()
find_package(OpenSSL REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/deezerapi.cpp
    src/deezerauth.cpp
    src/blowfish_jukebox.cpp
    src/audioengine.cpp
    src/streamdownloader.cpp
    src/playlist.cpp
    src/track.cpp
    src/album.cpp
    src/playlistwidget.cpp
    src/albumlistwidget.cpp
    src/searchwidget.cpp
    src/playercontrols.cpp
    src/tracklistwidget.cpp
    src/waveformwidget.cpp
    src/spectrumwidget.cpp
    src/spectrumwindow.cpp
    src/lyricswidget.cpp
    src/lyricswindow.cpp
    src/windowsmediacontrols.cpp
    src/discordmanager.cpp
    src/queueheaderwidget.cpp
    src/projectmwidget.cpp
    src/projectmwindow.cpp
    src/lastfmapi.cpp
    src/scrobblecache.cpp
    src/lastfmsettingsdialog.cpp
)

set(HEADERS
    src/mainwindow.h
    src/deezerapi.h
    src/deezerauth.h
    src/blowfish_jukebox.h
    src/audioengine.h
    src/streamdownloader.h
    src/playlist.h
    src/track.h
    src/album.h
    src/playlistwidget.h
    src/albumlistwidget.h
    src/searchwidget.h
    src/playercontrols.h
    src/tracklistwidget.h
    src/waveformwidget.h
    src/spectrumwidget.h
    src/spectrumwindow.h
    src/lyricswidget.h
    src/lyricswindow.h
    src/windowsmediacontrols.h
    src/discordmanager.h
    src/queueheaderwidget.h
    src/projectmwidget.h
    src/projectmwindow.h
    src/lastfmapi.h
    src/scrobblecache.h
    src/lastfmsettingsdialog.h
)

# Resources
set(RESOURCES
    resources/resources.qrc
)

# Create executable
qt_add_executable(${PROJECT_NAME}
    MANUAL_FINALIZATION
    ${SOURCES}
    ${HEADERS}
    ${RESOURCES}
)

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Finalize the target (required when using MANUAL_FINALIZATION)
qt_finalize_executable(${PROJECT_NAME})

# Link Qt libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::OpenGLWidgets
)

target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL)
target_compile_definitions(${PROJECT_NAME} PRIVATE DEEZER_OPENSSL=1)

# BASS audio library (you'll need to download this)
# Download from: https://www.un4seen.com/bass.html
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/external/bass
    ${CMAKE_SOURCE_DIR}/external/projectm/include
    ${CMAKE_SOURCE_DIR}/external/projectm/include/libprojectM
    C:/Users/Tambourin/source/repos/projectm/vcpkg_installed/x64-windows/include
)

# Link BASS library
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/external/bass/bass.lib
        ${CMAKE_SOURCE_DIR}/external/bass/bassmix.lib
        ${CMAKE_SOURCE_DIR}/external/projectm/libprojectM-4.lib
        ${CMAKE_SOURCE_DIR}/external/projectm/libprojectM_eval.lib
        ${CMAKE_SOURCE_DIR}/external/projectm/hlslparser.lib
        ${CMAKE_SOURCE_DIR}/external/projectm/glew32.lib
        opengl32.lib
        runtimeobject.lib
    )
endif()

# Copy BASS DLL to output directory
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/external/bass/bass.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/external/bass/bassmix.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/external/projectm/glew32.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )

    # projectM is statically linked, no DLL needed
endif()

# Set output directories
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Deploy Qt DLLs next to the executable on Windows (so the exe finds Qt6Core.dll, etc.)
if(WIN32)
    get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
    if(_qt_core_dll)
        get_filename_component(_qt_bin_dir "${_qt_core_dll}" DIRECTORY)
        get_filename_component(_qt_bin_dir "${_qt_bin_dir}/../bin" REALPATH)
        find_program(WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt windeployqt6
            HINTS "${_qt_bin_dir}" "$ENV{QT_DIR}/bin"
            DOC "Qt deployment tool"
        )
        if(WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}" "$<TARGET_FILE:${PROJECT_NAME}>"
                COMMENT "Deploying Qt DLLs with windeployqt..."
            )
        endif()
    endif()
endif()

# Installation
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)
