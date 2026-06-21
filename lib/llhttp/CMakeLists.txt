add_library( llhttp STATIC api.c http.c llhttp.c )
target_include_directories( llhttp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} )
