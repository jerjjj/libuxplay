find_package( PkgConfig REQUIRED )

pkg_check_modules( GLIB2 REQUIRED glib-2.0 )
if ( ${GLIB2_VERSION} VERSION_LESS "2.68" )
  message( STATUS "GLib2 < 2.68: Need g_string_replace replacement" )
  add_definitions( -DNEED_G_STRING_REPLACE )
endif()

pkg_check_modules( GST REQUIRED
  gstreamer-1.0>=1.4
  gstreamer-sdp-1.0>=1.4
  gstreamer-video-1.0>=1.4
  gstreamer-app-1.0>=1.4
)

add_library( renderers STATIC
  audio_renderer.c
  video_renderer.c
  mux_renderer.c
)

target_link_libraries( renderers PUBLIC airplay )
target_include_directories( renderers PUBLIC ${GST_INCLUDE_DIRS} )

if ( CMAKE_VERSION VERSION_LESS "3.12" )
  target_link_libraries( renderers PUBLIC ${GST_LIBRARIES} )
else()
  target_link_libraries( renderers PUBLIC ${GST_LINK_LIBRARIES} )
endif()
