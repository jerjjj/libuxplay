cmake_minimum_required( VERSION 3.13 )

project( libuxplay
  VERSION   1.0.0
  LANGUAGES C CXX
  DESCRIPTION "AirPlay Mirroring & Audio Server Library"
)

include( GNUInstallDirs )

set( CMAKE_CXX_STANDARD 14 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_C_STANDARD 11 )
set( CMAKE_POSITION_INDEPENDENT_CODE ON )

message( STATUS "=== libuxplay ${PROJECT_VERSION} ===" )

# ---- Options ----
option( BUILD_SHARED_LIBS "Build shared library (DLL)" ON )
option( BUILD_EXAMPLES    "Build example programs"     ON )

# ---- Platform setup ----
if( WIN32 )
  set( CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE )
  add_definitions( -DFULL_RANGE_RGB_FIX )
elseif( UNIX AND NOT APPLE )
  add_definitions( -DSUPPRESS_AVAHI_COMPAT_WARNING -DFULL_RANGE_RGB_FIX )
endif()

# ---- Build sub-libraries ----
add_subdirectory( lib )
add_subdirectory( renderers )

# ---- uxplaylib (shared / static) ----
if( BUILD_SHARED_LIBS )
  add_library( uxplaylib SHARED src/uxplaylib.cpp )
else()
  add_library( uxplaylib STATIC src/uxplaylib.cpp )
endif()

target_compile_definitions( uxplaylib PRIVATE UXPLAY_BUILDING_DLL )

target_include_directories( uxplaylib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries( uxplaylib PRIVATE renderers airplay )

if( WIN32 )
  target_link_libraries( uxplaylib PRIVATE wsock32 ws2_32 iphlpapi )
endif()

set_target_properties( uxplaylib PROPERTIES
  OUTPUT_NAME   "uxplaylib"
  VERSION       ${PROJECT_VERSION}
  SOVERSION     ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS OFF
  PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/uxplaylib.h"
)

# ---- Examples ----
if( BUILD_EXAMPLES )
  add_executable( test_uxplaylib examples/test_uxplaylib.c )
  target_link_libraries( test_uxplaylib PRIVATE uxplaylib )
endif()

# ---- Install ----
install( TARGETS uxplaylib
  EXPORT  uxplaylibTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install( EXPORT uxplaylibTargets
  FILE        uxplaylibConfig.cmake
  NAMESPACE   uxplaylib::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uxplaylib
)

if( BUILD_EXAMPLES )
  install( TARGETS test_uxplaylib RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} )
endif()

install( FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} )
install( FILES lib/llhttp/LICENSE-MIT DESTINATION ${CMAKE_INSTALL_DOCDIR}/llhttp )
