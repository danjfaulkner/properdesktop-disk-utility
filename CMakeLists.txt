# SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

project(properdesktop-disk-utility VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CTest)

find_package(ProperDesktopToolkit REQUIRED)

add_library(disk-utility-core STATIC
    src/actions.cpp
    src/application.cpp
    src/dialogs/erase_plan.cpp
    src/dialogs/partition_plan.cpp
    src/dialogs/plan_runner.cpp
    src/dialogs/volume_format.cpp
    src/images/blankimage.cpp
    src/images/copy.cpp
    src/images/formats.cpp
    src/images/imagefile.cpp
    src/images/progress.cpp
    src/images/size.cpp
    src/images/transfer.cpp
    src/udisks/bus.cpp
    src/udisks/describe.cpp
    src/udisks/errors.cpp
    src/udisks/guard.cpp
    src/udisks/holders.cpp
    src/udisks/identity.cpp
    src/udisks/model.cpp
    src/udisks/operations.cpp
    src/udisks/parse.cpp
    src/udisks/properties.cpp
    src/udisks/smart.cpp
    src/udisks/system.cpp
    src/udisks/tree.cpp
)
target_include_directories(disk-utility-core PUBLIC src)
target_link_libraries(disk-utility-core PUBLIC ProperDesktop::ToolkitBase)

# The glyphs, built in.
file(GLOB DISK_UTILITY_ICONS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/icons/*.svg)
set(DISK_UTILITY_EMBEDDED ${CMAKE_CURRENT_BINARY_DIR}/icons.cpp)
# A list would be split into separate arguments on the way to the script.
string(REPLACE ";" "|" DISK_UTILITY_ICON_LIST "${DISK_UTILITY_ICONS}")
add_custom_command(OUTPUT ${DISK_UTILITY_EMBEDDED}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${DISK_UTILITY_EMBEDDED}
            -DFILES=${DISK_UTILITY_ICON_LIST} -P ${PROJECT_SOURCE_DIR}/cmake/EmbedIcons.cmake
    DEPENDS ${DISK_UTILITY_ICONS} ${PROJECT_SOURCE_DIR}/cmake/EmbedIcons.cmake
    VERBATIM)

add_library(disk-utility-ui STATIC
    src/dialogs/cards.cpp
    src/dialogs/choice_button.cpp
    src/dialogs/erase.cpp
    src/dialogs/firstaid.cpp
    src/dialogs/identity_guard.cpp
    src/dialogs/info.cpp
    src/dialogs/mount.cpp
    src/dialogs/partition.cpp
    src/dialogs/pie_chart.cpp
    src/dialogs/rename.cpp
    src/dialogs/smart.cpp
    src/dialogs/unlock.cpp
    src/dialogs/volume_actions.cpp
    src/dialogs/volumetools.cpp
    src/dialogs/copyprogress.cpp
    src/dialogs/diskimages.cpp
    src/dialogs/newimage.cpp
    src/dialogs/restore.cpp
    src/views/context.cpp
    src/views/detail.cpp
    src/views/detailcards.cpp
    src/views/detailcontent.cpp
    src/views/diskwindow.cpp
    src/views/sidebar.cpp
    src/views/style.cpp
    src/views/toolbar.cpp
    ${DISK_UTILITY_EMBEDDED}
)
target_include_directories(disk-utility-ui PUBLIC src)
target_link_libraries(disk-utility-ui PUBLIC disk-utility-core ProperDesktop::Toolkit)

add_executable(${PROJECT_NAME} src/main.cpp)
target_compile_definitions(${PROJECT_NAME} PRIVATE DISK_UTILITY_VERSION="${PROJECT_VERSION}")
target_link_libraries(${PROJECT_NAME} PRIVATE disk-utility-ui)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

properdesktop_add_translations(properdesktop-disk-utility SOURCES src DESKTOP_FILES properdesktop-disk-utility.desktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/properdesktop-disk-utility.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
