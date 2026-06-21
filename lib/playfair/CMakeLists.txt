add_library( playfair STATIC
    hand_garble.c modified_md5.c omg_hax.c playfair.c sap_hash.c )
target_include_directories( playfair PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} )
