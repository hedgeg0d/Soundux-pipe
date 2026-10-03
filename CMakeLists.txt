cmake_minimum_required(VERSION 3.5)

# [[ The vendored dependencies still declare minimum versions that current CMake refuses ]]
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(soundux VERSION 0.2.8 DESCRIPTION "A crossplatform soundboard")

set(FULL_VERSION_STRING "0.2.8")
option(EMBED_PATH "The path used for embedding" "OFF")
option(USE_FLATPAK "Allows the program to run under flatpak" OFF)
option(SYSTEM_INSTALL "Install into the system directories instead of /opt/soundux" OFF)
option(SOUNDUX_BUILD_TESTS "Build Soundux regression tests" OFF)

file(GLOB src
    "src/*.cpp"
    "src/*/*.cpp"
    "src/*/*/*.cpp"
    "src/*/*/*/*.cpp"
    "src/*/*/*/*/*.cpp"
)

if (WIN32)
    add_executable(soundux WIN32 ${src} "assets/icon.rc")

    set(CompilerFlags
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_C_FLAGS_RELWITHDEBINFO
    )

    foreach(CompilerFlag ${CompilerFlags})
        string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
        set(${CompilerFlag} "${${CompilerFlag}}" CACHE STRING "msvc compiler flags" FORCE)
        message("MSVC flags: ${CompilerFlag}:${${CompilerFlag}}")
    endforeach()

    target_compile_definitions(soundux PRIVATE WIN32_LEAN_AND_MEAN=1)
    target_compile_options(soundux PRIVATE /W4)
else()
    add_executable(soundux ${src})

    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(STATUS "Enabling warning and error flags for debug build")
        target_compile_options(soundux PRIVATE -Wall -Werror -Wextra -pedantic -Wno-unused-lambda-capture -Wno-gnu)
    endif()
endif()

target_compile_definitions(soundux PRIVATE SOUNDUX_VERSION="${FULL_VERSION_STRING}" WNCK_I_KNOW_THIS_IS_UNSTABLE=1)
target_include_directories(soundux SYSTEM PRIVATE "src")
target_include_directories(soundux SYSTEM PRIVATE "lib/miniaudio")
target_include_directories(soundux SYSTEM PRIVATE "lib/semver/include")
target_include_directories(soundux SYSTEM PRIVATE "lib/fancypp/include")
target_include_directories(soundux SYSTEM PRIVATE "lib/json/single_include")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(soundux PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

if (UNIX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})
    find_package(PipeWire REQUIRED)

    find_package(PulseAudio)
    find_package(X11 REQUIRED)
    target_include_directories(soundux SYSTEM PRIVATE ${X11_INCLUDE_DIR} ${PULSEAUDIO_INCLUDE_DIR} ${PipeWire_INCLUDE_DIRS} ${Spa_INCLUDE_DIRS}) 

    find_package(PkgConfig REQUIRED)
	pkg_check_modules(WNCK libwnck-3.0)
    target_link_libraries(soundux PRIVATE ${X11_LIBRARIES} ${X11_Xinput_LIB} ${X11_XTest_LIB})
endif()
if (WIN32)
    target_compile_definitions(soundux PRIVATE _CRT_SECURE_NO_WARNINGS=1 _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS=1 _UNICODE=1)
endif()

# [[ webkit2gtk-4.0 was renamed to 4.1, the vendored webview still asks for the old name ]]
if (UNIX)
    pkg_check_modules(WEBKIT2GTK_41 QUIET webkit2gtk-4.1)

    #* Prefer 4.1 whenever it is available, a leftover 4.0 usually is not usable anyway
    if (WEBKIT2GTK_41_FOUND)
        set(WEBKIT2GTK_COMPAT_DIR "${CMAKE_BINARY_DIR}/webkit2gtk-compat")
        file(WRITE "${WEBKIT2GTK_COMPAT_DIR}/webkit2gtk-4.0.pc"
            "Name: webkit2gtk-4.0\n"
            "Description: Alias for webkit2gtk-4.1\n"
            "Version: 4.1\n"
            "Requires: webkit2gtk-4.1\n")
        set(ENV{PKG_CONFIG_PATH} "${WEBKIT2GTK_COMPAT_DIR}:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()

add_subdirectory(src/ui/impl/webview/lib/webviewpp EXCLUDE_FROM_ALL)
add_subdirectory(lib/nativefiledialog-extended EXCLUDE_FROM_ALL)
add_subdirectory(lib/tiny-process-library EXCLUDE_FROM_ALL)
add_subdirectory(lib/backward-cpp EXCLUDE_FROM_ALL)
add_subdirectory(lib/traypp EXCLUDE_FROM_ALL)
add_subdirectory(lib/guardpp)
add_subdirectory(lib/lockpp)

add_backward(soundux)

if (WIN32)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    set(OPENSSL_MSVC_STATIC_RT  TRUE)
    set(BROTLI_USE_STATIC_LIBS TRUE)
endif()

set(HTTPLIB_REQUIRE_OPENSSL ON)
add_subdirectory(lib/cpp-httplib EXCLUDE_FROM_ALL)
target_include_directories(soundux SYSTEM PRIVATE "lib/cpp-httplib")

target_link_libraries(soundux PRIVATE webview nfd tiny-process-library tray guardpp httplib lockpp)

# [[ we use gtk directly for the window key events, the webview does not export its own link line ]]
if (UNIX)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    target_include_directories(soundux SYSTEM PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(soundux PRIVATE ${GTK3_LIBRARIES})
endif ()

if (${EMBED_PATH} STREQUAL "OFF")
    message("Web-content will not be embedded") 
    
    # [[ Copy required files ]]
    #  > Copies frontend files
    set(SOUNDUX_FRONTEND_DIR "${CMAKE_BINARY_DIR}/dist")
    file(COPY "${CMAKE_SOURCE_DIR}/src/ui/impl/webview/lib/soundux-ui/"
         DESTINATION "${SOUNDUX_FRONTEND_DIR}")
    include(cmake/PatchFrontend.cmake)
    soundux_patch_frontend("${SOUNDUX_FRONTEND_DIR}")
    if(MSVC)
        add_custom_command(TARGET soundux POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${SOUNDUX_FRONTEND_DIR}" "$<TARGET_FILE_DIR:soundux>/dist")
    endif()
else()
    message("Using embed path " ${EMBED_PATH})
    target_include_directories(webview PUBLIC ${EMBED_PATH})
    target_compile_definitions(soundux PRIVATE IS_EMBEDDED=1)
endif()

target_compile_features(soundux PRIVATE cxx_std_17)
set_target_properties(soundux PROPERTIES 
                      CXX_STANDARD 17
                      CXX_EXTENSIONS OFF
                      CXX_STANDARD_REQUIRED ON)

set_target_properties(soundux PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(soundux PROPERTIES PROJECT_NAME ${PROJECT_NAME})

if(SOUNDUX_BUILD_TESTS AND UNIX)
    enable_testing()
    set(test_sources ${src})
    list(REMOVE_ITEM test_sources "${CMAKE_SOURCE_DIR}/src/main.cpp"
                                "${CMAKE_SOURCE_DIR}/src/ui/impl/webview/webview.cpp")
    add_executable(soundux-regression tests/regression.cpp ${test_sources})
    get_target_property(soundux_includes soundux INCLUDE_DIRECTORIES)
    get_target_property(soundux_definitions soundux COMPILE_DEFINITIONS)
    get_target_property(soundux_libraries soundux LINK_LIBRARIES)
    target_include_directories(soundux-regression SYSTEM PRIVATE ${soundux_includes})
    target_compile_definitions(soundux-regression PRIVATE ${soundux_definitions})
    target_link_libraries(soundux-regression PRIVATE ${soundux_libraries})
    target_compile_features(soundux-regression PRIVATE cxx_std_17)
    set_target_properties(soundux-regression PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME config-regression COMMAND ${CMAKE_COMMAND} -E env
             "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config" $<TARGET_FILE:soundux-regression>)
    find_program(NODE_EXECUTABLE node)
    if(NODE_EXECUTABLE AND SOUNDUX_FRONTEND_DIR)
        add_test(NAME frontend-regression COMMAND ${NODE_EXECUTABLE}
                 "${CMAKE_SOURCE_DIR}/tests/frontend.js" "${SOUNDUX_FRONTEND_DIR}")
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(PIPEWIRE_EXECUTABLE pipewire)
    find_program(WIREPLUMBER_EXECUTABLE wireplumber)
    find_program(PW_DUMP_EXECUTABLE pw-dump)
    find_program(PW_METADATA_EXECUTABLE pw-metadata)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
    if(Python3_FOUND AND PIPEWIRE_EXECUTABLE AND WIREPLUMBER_EXECUTABLE AND
       PW_DUMP_EXECUTABLE AND PW_METADATA_EXECUTABLE AND DBUS_RUN_SESSION_EXECUTABLE)
        find_program(XVFB_EXECUTABLE Xvfb)
        set(desktop_test_argument "")
        if(XVFB_EXECUTABLE)
            set(desktop_test_argument $<TARGET_FILE:soundux>)
        endif()
        add_test(NAME pipewire-regression COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} --
                 ${Python3_EXECUTABLE} -B "${CMAKE_SOURCE_DIR}/tests/pipewire.py"
                 $<TARGET_FILE:soundux-regression> ${desktop_test_argument})
        set_tests_properties(pipewire-regression PROPERTIES TIMEOUT 90)
    endif()
endif()


if(USE_FLATPAK)
    target_compile_definitions(soundux PRIVATE USE_FLATPAK)
    target_link_libraries(soundux PRIVATE ${PULSEAUDIO_LIBRARY})

    install(TARGETS soundux DESTINATION bin)
    if (${EMBED_PATH} STREQUAL "OFF")
        install(DIRECTORY "${SOUNDUX_FRONTEND_DIR}" DESTINATION bin)
    endif()
elseif(SYSTEM_INSTALL)
  # [[ Install into the system directories ]]
  #  > Distributions use this so that no launcher next to the binary is required
  include(GNUInstallDirs)

  install(TARGETS soundux DESTINATION ${CMAKE_INSTALL_BINDIR})
  if (${EMBED_PATH} STREQUAL "OFF")
    install(DIRECTORY "${SOUNDUX_FRONTEND_DIR}" DESTINATION ${CMAKE_INSTALL_DATADIR}/soundux)
  endif()

  #  > The desktop entry has to start the binary from the binary directory
  file(READ "${CMAKE_SOURCE_DIR}/deployment/soundux.desktop" sounduxDesktopEntry)
  string(REPLACE "Exec=/opt/soundux/soundux" "Exec=soundux" sounduxDesktopEntry "${sounduxDesktopEntry}")
  file(WRITE "${CMAKE_BINARY_DIR}/soundux.desktop" "${sounduxDesktopEntry}")

  install(FILES "${CMAKE_BINARY_DIR}/soundux.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES "${CMAKE_SOURCE_DIR}/deployment/appstream/io.github.Soundux.metainfo.xml"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  install(FILES "${CMAKE_SOURCE_DIR}/assets/soundux.png" DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps)

  target_compile_definitions(soundux PRIVATE SOUNDUX_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/soundux")
else()
  set(CMAKE_INSTALL_PREFIX "/opt/soundux" CACHE PATH "Install path prefix, prepended onto install directories." FORCE)
  install(TARGETS soundux DESTINATION .)
  if (${EMBED_PATH} STREQUAL "OFF")
    install(DIRECTORY "${SOUNDUX_FRONTEND_DIR}" DESTINATION .)
  endif()
  install(FILES "${CMAKE_SOURCE_DIR}/deployment/soundux.desktop" DESTINATION /usr/share/applications)
  install(FILES "${CMAKE_SOURCE_DIR}/deployment/appstream/io.github.Soundux.metainfo.xml" DESTINATION /usr/share/metainfo)
  install(FILES "${CMAKE_SOURCE_DIR}/assets/soundux.png" DESTINATION /usr/share/pixmaps)
endif()
