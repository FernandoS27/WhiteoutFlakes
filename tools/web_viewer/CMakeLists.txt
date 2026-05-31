# web_viewer — demo host for the whiteout-js-viewer npm package.
# Stages index.html + sw.js into ${CMAKE_BINARY_DIR}/web/ alongside the
# whiteout-js-viewer/ dir that packages/whiteout-js-viewer/ produces.
#
# Doesn't define wf_web — that target lives in the package's CMakeLists.

if(NOT TARGET wf_web)
    message(FATAL_ERROR
        "tools/web_viewer expects target 'wf_web' (defined in "
        "packages/whiteout-js-viewer/CMakeLists.txt) to already exist. "
        "Make sure packages/whiteout-js-viewer is added before tools/web_viewer.")
endif()

# Demo-only HTML/SW assets — everything import-able lives in the package.
# Prebuilt-shader staging is bound to wf_web POST_BUILD and lives in the
# package's CMakeLists (CMake requires custom-command-on-target to be in
# the same directory the target was defined).
foreach(_f index.html sw.js)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${_f}"
                   "${CMAKE_BINARY_DIR}/web/${_f}" COPYONLY)
endforeach()
