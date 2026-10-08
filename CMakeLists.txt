cmake_minimum_required(VERSION 3.24)
project(
    swak
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network OpenGLWidgets Concurrent)
find_package(LibArchive REQUIRED)
find_package(GDAL CONFIG REQUIRED)
if(GDAL_VERSION VERSION_LESS 3.6)
    message(FATAL_ERROR "SWAK requires GDAL 3.6 or later. Found ${GDAL_VERSION}.")
endif()
find_path(GEOGRAPHICLIB_INCLUDE_DIR GeographicLib/UTMUPS.hpp REQUIRED)
find_library(GEOGRAPHICLIB_LIBRARY NAMES GeographicLib REQUIRED)
include(CTest)
include(GNUInstallDirs)
option(SWAK_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SWAK_COVERAGE "Enable GCC coverage for development" OFF)
if(SWAK_SANITIZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Sanitizers require GCC or Clang.")
endif()
if(SWAK_COVERAGE AND (NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR SWAK_SANITIZERS))
    message(FATAL_ERROR "Coverage requires GCC and a separate build without sanitizers.")
endif()

add_library(swak_build INTERFACE)
target_compile_features(swak_build INTERFACE cxx_std_20)
set(version_command
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/generated/swak/version.h"
    "-DBASE_VERSION=${PROJECT_VERSION}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake"
)
execute_process(COMMAND ${version_command} COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(
    swak_version
    COMMAND ${version_command}
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/swak/version.h"
    VERBATIM
)
add_dependencies(swak_build swak_version)
target_include_directories(swak_build INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(
    swak_build
    INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-fstack-protector-strong>
)
target_link_options(swak_build INTERFACE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,-z,relro,-z,now>)
target_compile_options(
    swak_build
    INTERFACE
        $<$<AND:$<CONFIG:Release,RelWithDebInfo>,$<CXX_COMPILER_ID:GNU,Clang>>:-include;${CMAKE_CURRENT_SOURCE_DIR}/src/core/hardening.h>
)
if(SWAK_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        swak_build INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(swak_build INTERFACE -fsanitize=address,undefined)
endif()
if(SWAK_COVERAGE)
    target_compile_options(swak_build INTERFACE --coverage -O0 -g)
    target_link_options(swak_build INTERFACE --coverage)
endif()

add_library(
    swak_core STATIC
    src/core/credits.cpp
    src/core/credits.h
    src/core/graphics.cpp
    src/core/graphics.h
    src/core/measure.cpp
    src/core/measure.h
    src/core/lan_protocol.cpp
    src/core/prediction.cpp
    src/core/track_motion.cpp
    src/core/settings_dialog.cpp
    src/core/server_browser.cpp
    src/core/cot.cpp
    src/core/cot_detail.cpp
    src/core/cot_detail.h
    src/core/tls_files.cpp
    src/core/tls_files.h
    src/core/network_client.cpp
    src/core/network_client.h
    src/core/tak_server_api.cpp
    src/core/tak_server_api.h
    src/core/tak_enrollment.cpp
    src/core/tak_enrollment.h
    src/core/quic_stream.cpp
    src/core/quic_stream.h
    src/core/tak_protocol.cpp
    src/core/tak_protocol.h
    src/core/emergency.cpp
    src/core/emergency.h
    src/core/tak_shapes.cpp
    src/core/tak_shapes.h
    src/core/tak_drawings.cpp
    src/core/tak_drawings.h
    src/core/tak_channels.cpp
    src/core/tak_channels.h
    src/core/chat.cpp
    src/core/data_package.cpp
    src/core/file_shares.cpp
    src/core/file_shares.h
    src/core/missions.cpp
    src/core/missions.h
    src/core/data_package.h
    src/core/package_import.cpp
    src/core/package_import.h
    src/core/chat.h
    src/core/contacts.cpp
    src/core/contacts.h
    src/core/object_sharing.cpp
    src/core/object_sharing.h
    src/core/cot_stream.cpp
    src/core/identity.cpp
    src/core/servers.cpp
    src/core/ui_icons.cpp
    src/core/ui_icons.h
    src/core/polygon.cpp
    src/core/polygon.h
    src/core/map_auth.cpp
    src/core/marker_collections.cpp
    src/core/marker_collections.h
    src/core/milstd2525_symbols.cpp
    src/core/marker_symbols.cpp
    src/core/marker_symbols.h
    src/core/grid_types.cpp
    src/core/grid_types.h
    src/core/map_decorations.cpp
    src/core/map_decorations.h
    src/core/map_grid.cpp
    src/core/map_grid.h
    src/core/object_editor.cpp
    src/core/object_editor.h
    src/core/user_objects.cpp
    src/core/user_objects.h
    src/core/map_tools.cpp
    src/core/map_tools.h
    src/core/map_selection.cpp
    src/core/map_selection.h
    src/core/vector_tiles.cpp
    src/core/vector_tiles.h
    src/core/data.cpp
    src/core/data.h
    src/core/event.cpp
    src/core/event.h
    src/core/files.cpp
    src/core/files.h
    src/core/gdal_support.cpp
    src/core/gdal_support.h
    src/core/interfaces.h
    src/core/single_line_label.h
    src/core/plugin_catalog.cpp
    src/core/plugin_catalog.h
    src/core/activity.cpp
    src/core/activity.h
    src/core/store.cpp
    src/core/store.h
    src/core/projection.cpp
    src/core/projection.h
    src/core/preferences.cpp
    src/core/preferences.h
    src/core/settings.cpp
    src/core/settings.h
    src/core/cot_picker.cpp
    src/core/cot_picker.h
    src/core/routing.cpp
    src/core/routing.h
    src/core/maps.cpp
    src/core/maps.h
    src/core/tiles.cpp
    src/core/tiles.h
    src/core/logging.cpp
    src/core/logging.h
)
# TAK Protocol version 1 uses SWAK's own schema and the Protobuf lite runtime. The generated code
# builds in its own library without the SWAK warning flags. Its header is a system header.
find_package(Protobuf 3.12 REQUIRED)
add_library(swak_tak_proto STATIC src/core/proto/tak_protocol.proto)
protobuf_generate(
    TARGET
    swak_tak_proto
    LANGUAGE
    cpp
    IMPORT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/proto"
    PROTOC_OUT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/proto"
)
target_include_directories(
    swak_tak_proto SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated/proto"
)
target_compile_features(swak_tak_proto PUBLIC cxx_std_20)
target_link_libraries(swak_tak_proto PUBLIC protobuf::libprotobuf-lite)
if(SWAK_SANITIZERS)
    target_compile_options(
        swak_tak_proto PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
    )
endif()
target_include_directories(swak_core PUBLIC src)
target_link_libraries(
    swak_core PUBLIC swak_build LibArchive::LibArchive Qt6::Widgets Qt6::Network Qt6::Concurrent
)

target_include_directories(swak_core SYSTEM PRIVATE "${GEOGRAPHICLIB_INCLUDE_DIR}")
target_link_libraries(swak_core PRIVATE GDAL::GDAL)
# The HTTPS client needs CURLOPT_PROTOCOLS_STR, which libcurl 7.85 added.
find_package(CURL REQUIRED)
if(CURL_VERSION_STRING VERSION_LESS 7.85)
    message(FATAL_ERROR "SWAK needs libcurl 7.85 or later. Found ${CURL_VERSION_STRING}.")
endif()
target_link_libraries(swak_core PRIVATE CURL::libcurl)
# TAK certificate enrollment creates keys and certificate requests with OpenSSL 3. TAK QUIC streams
# use the OpenSSL QUIC client, which OpenSSL 3.2 added. 3.5 adds the server side that the tests use.
find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto SSL)
target_link_libraries(swak_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(swak_core PRIVATE swak_tak_proto)
target_link_libraries(swak_core PUBLIC "${GEOGRAPHICLIB_LIBRARY}")

function(swak_plugin name file)
    add_library(${name} STATIC ${file})
    target_compile_definitions(${name} PRIVATE QT_STATICPLUGIN)
    target_link_libraries(${name} PUBLIC swak_core ${ARGN})
endfunction()
swak_plugin(swak_gdal_vector src/plugins/gdal_vector.cpp GDAL::GDAL)
swak_plugin(swak_mvt src/plugins/mvt.cpp GDAL::GDAL)
swak_plugin(swak_gdal_raster src/plugins/gdal_raster.cpp GDAL::GDAL)
swak_plugin(swak_google_maps src/plugins/google_maps.cpp)
swak_plugin(swak_azure_maps src/plugins/azure_maps.cpp)
swak_plugin(swak_emergencies src/plugins/emergencies.cpp)
swak_plugin(swak_contacts src/plugins/contacts.cpp)
swak_plugin(swak_sharing src/plugins/sharing.cpp)
swak_plugin(swak_takdrawings src/plugins/takdrawings.cpp)
swak_plugin(swak_chat src/plugins/chat.cpp)
swak_plugin(swak_files src/plugins/files.cpp)
swak_plugin(swak_missions src/plugins/missions.cpp)
swak_plugin(swak_milstd2525 src/plugins/milstd2525.cpp)
swak_plugin(swak_polygons src/plugins/polygons.cpp)
swak_plugin(swak_markers src/plugins/markers.cpp)
swak_plugin(swak_information src/plugins/information.cpp)
swak_plugin(swak_activity src/plugins/activity.cpp)
swak_plugin(swak_cot src/plugins/cot.cpp)
swak_plugin(swak_map src/plugins/map.cpp Qt6::OpenGLWidgets)
swak_plugin(swak_basemap src/plugins/basemap.cpp)
swak_plugin(swak_trace src/plugins/trace.cpp)
swak_plugin(swak_predict src/plugins/predict.cpp)
swak_plugin(swak_shapes src/plugins/shapes.cpp)
swak_plugin(swak_valhalla src/plugins/valhalla.cpp)
swak_plugin(swak_route_planner src/plugins/route_planner.cpp)
swak_plugin(swak_objects src/plugins/objects.cpp)
swak_plugin(swak_tracks src/plugins/tracks.cpp)
swak_plugin(swak_servers src/plugins/servers.cpp Qt6::Network)
swak_plugin(swak_editor src/plugins/editor.cpp)

add_executable(
    swak
    src/app/main.cpp
    src/app/window.cpp
    src/app/window.h
    src/app/map_panels.cpp
    src/app/map_panels.h
    src/app/map_manager.cpp
    src/app/map_manager.h
    src/app/data_manager.cpp
    src/app/data_manager.h
    src/app/marker_manager.cpp
    src/app/marker_manager.h
    src/app/plugins.cpp
)
target_link_libraries(
    swak
    PRIVATE swak_shapes
            swak_trace
            swak_predict
            swak_milstd2525
            swak_polygons
            swak_markers
            swak_information
            swak_mvt
            swak_gdal_vector
            swak_activity
            swak_cot
            swak_map
            swak_basemap
            swak_valhalla
            swak_route_planner
            swak_objects
            swak_tracks
            swak_servers
            swak_editor
            swak_gdal_raster
            swak_google_maps
            swak_azure_maps
            swak_emergencies
            swak_contacts
            swak_sharing
            swak_takdrawings
            swak_chat
            swak_files
            swak_missions
)
install(TARGETS swak RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/org.swak.Swak.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/org.swak.Swak.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)
install(
    FILES docs/GeographicLib-copyright.txt
          docs/routing.md
          docs/servers.md
          docs/usage.md
          docs/map-providers.md
          docs/data-providers.md
          docs/dependencies.md
          docs/atak-reference-review.md
          docs/marker-collections.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    add_library(swak_simulation STATIC tools/cot_simulation.cpp tools/cot_simulation.h)
    target_link_libraries(swak_simulation PUBLIC swak_core)
    target_include_directories(swak_simulation PUBLIC tools)
    add_executable(swak_simulation_tests tests/test_simulation.cpp)
    target_link_libraries(swak_simulation_tests PRIVATE swak_simulation Qt6::Test)
    add_dependencies(swak_simulation_tests swak_cot_emulator)
    add_test(NAME simulation COMMAND swak_simulation_tests)
    set_tests_properties(simulation PROPERTIES TIMEOUT 60)
    add_executable(swak_cot_emulator tools/cot_emulator.cpp)
    target_link_libraries(swak_cot_emulator PRIVATE swak_simulation)
    add_executable(swak_tak_compat tools/tak_compat.cpp)
    target_link_libraries(swak_tak_compat PRIVATE swak_core Qt6::Test)
    add_executable(swak_server_tests tests/test_servers.cpp)
    target_link_libraries(
        swak_server_tests PRIVATE swak_core swak_tak_proto Qt6::Test OpenSSL::SSL OpenSSL::Crypto
    )
    add_test(NAME servers COMMAND swak_server_tests)
    set_tests_properties(servers PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(swak_vector_tests tests/test_vector.cpp)
    target_link_libraries(swak_vector_tests PRIVATE swak_mvt Qt6::Test)
    add_test(NAME vector COMMAND swak_vector_tests)
    set_tests_properties(vector PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(swak_data_tests tests/test_data.cpp)
    target_link_libraries(swak_data_tests PRIVATE swak_gdal_vector Qt6::Test)
    add_test(NAME data COMMAND swak_data_tests)
    set_tests_properties(data PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(swak_map_tests tests/test_maps.cpp src/app/plugins.cpp)
    target_link_libraries(
        swak_map_tests
        PRIVATE swak_shapes
                swak_trace
                swak_predict
                swak_milstd2525
                swak_polygons
                swak_markers
                swak_information
                swak_mvt
                swak_gdal_vector
                swak_activity
                swak_cot
                swak_map
                swak_basemap
                swak_valhalla
                swak_route_planner
                swak_objects
                swak_tracks
                swak_servers
                swak_editor
                swak_gdal_raster
                swak_google_maps
                swak_azure_maps
                swak_emergencies
                swak_contacts
                swak_sharing
                swak_takdrawings
                swak_chat
                swak_files
                swak_missions
                Qt6::Test
    )
    add_test(NAME maps COMMAND swak_map_tests)
    set_tests_properties(maps PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(swak_tests tests/test_core.cpp src/app/plugins.cpp)
    target_link_libraries(
        swak_tests
        PRIVATE swak_shapes
                swak_trace
                swak_predict
                swak_milstd2525
                swak_polygons
                swak_markers
                swak_information
                swak_mvt
                swak_gdal_vector
                swak_activity
                swak_cot
                swak_map
                swak_basemap
                swak_valhalla
                swak_route_planner
                swak_objects
                swak_tracks
                swak_servers
                swak_editor
                swak_gdal_raster
                swak_google_maps
                swak_azure_maps
                swak_emergencies
                swak_contacts
                swak_sharing
                swak_takdrawings
                swak_chat
                swak_files
                swak_missions
                Qt6::Test
                Qt6::Network
    )
    add_test(NAME core COMMAND swak_tests)
    set_tests_properties(core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(swak_activity_tests tests/test_activity.cpp)
    target_link_libraries(swak_activity_tests PRIVATE swak_core Qt6::Test)
    add_test(NAME activity COMMAND swak_activity_tests)
    set_tests_properties(activity PROPERTIES TIMEOUT 60)
    add_executable(swak_logging_tests tests/test_logging.cpp)
    target_link_libraries(swak_logging_tests PRIVATE swak_core Qt6::Test)
    add_test(NAME logging COMMAND swak_logging_tests)
    set_tests_properties(logging PROPERTIES TIMEOUT 60)
    add_executable(swak_preferences_tests tests/test_preferences.cpp)
    target_link_libraries(swak_preferences_tests PRIVATE swak_core Qt6::Test)
    add_test(NAME preferences COMMAND swak_preferences_tests)
    add_executable(
        swak_soak_tests
        tests/test_soak.cpp
        src/app/window.cpp
        src/app/window.h
        src/app/map_panels.cpp
        src/app/map_panels.h
        src/app/map_manager.cpp
        src/app/map_manager.h
        src/app/data_manager.cpp
        src/app/data_manager.h
        src/app/marker_manager.cpp
        src/app/marker_manager.h
        src/app/plugins.cpp
    )
    target_link_libraries(
        swak_soak_tests
        PRIVATE swak_shapes
                swak_trace
                swak_predict
                swak_milstd2525
                swak_polygons
                swak_markers
                swak_information
                swak_mvt
                swak_gdal_vector
                swak_activity
                swak_cot
                swak_map
                swak_basemap
                swak_valhalla
                swak_route_planner
                swak_objects
                swak_tracks
                swak_servers
                swak_editor
                swak_gdal_raster
                swak_google_maps
                swak_azure_maps
                swak_emergencies
                swak_contacts
                swak_sharing
                swak_takdrawings
                swak_chat
                swak_files
                swak_missions
                Qt6::Test
                Qt6::Network
    )
    add_test(NAME soak COMMAND swak_soak_tests)
    set_tests_properties(
        soak PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 LABELS "stress"
    )
    set_tests_properties(core preferences PROPERTIES TIMEOUT 60)
    add_executable(
        swak_gui_tests
        tests/test_gui.cpp
        src/app/window.cpp
        src/app/window.h
        src/app/map_panels.cpp
        src/app/map_panels.h
        src/app/map_manager.cpp
        src/app/map_manager.h
        src/app/data_manager.cpp
        src/app/data_manager.h
        src/app/marker_manager.cpp
        src/app/marker_manager.h
        src/app/plugins.cpp
    )
    add_dependencies(swak_gui_tests swak_cot_emulator)
    target_link_libraries(
        swak_gui_tests
        PRIVATE swak_shapes
                swak_trace
                swak_predict
                swak_milstd2525
                swak_polygons
                swak_markers
                swak_information
                swak_mvt
                swak_gdal_vector
                swak_activity
                swak_cot
                swak_map
                swak_basemap
                swak_valhalla
                swak_route_planner
                swak_objects
                swak_tracks
                swak_servers
                swak_editor
                swak_gdal_raster
                swak_google_maps
                swak_azure_maps
                swak_emergencies
                swak_contacts
                swak_sharing
                swak_takdrawings
                swak_chat
                swak_files
                swak_missions
                Qt6::Test
                Qt6::Network
                Qt6::OpenGLWidgets
    )
endif()

install(FILES third_party/lucide/LICENSE third_party/lucide/LICENSE.Feather
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/lucide
)
