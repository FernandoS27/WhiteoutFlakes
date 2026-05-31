# whiteout-js-viewer — npm-publishable package whose JS modules wrap the
# wf-core Emscripten runtime. This CMakeLists builds wf-core.{js,wasm}
# from the C++ sources under native/ and stages the publish layout into
# ${CMAKE_BINARY_DIR}/web/whiteout-js-viewer/:
#
#   whiteout-js-viewer/
#     package.json
#     index.js, hive-app.js, wf-*.js, web-audio.js   (copied from this dir)
#     wf-core.js, wf-core.wasm                       (built by wf_web)
#
# The directory above is what gets `npm publish`'d. The web_viewer demo
# (tools/web_viewer/) copies index.html + sw.js next to it for the dev
# server, but no build step there is required to ship the package.
#
# Only entered when EMSCRIPTEN is the configured toolchain — the package
# has no meaning on native builds.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "packages/whiteout-js-viewer/CMakeLists.txt added with a non-Emscripten toolchain. "
        "Guard the add_subdirectory() with if(EMSCRIPTEN).")
endif()

set(_wf_pkg_dir "${CMAKE_BINARY_DIR}/web/whiteout-js-viewer")

add_executable(wf_web
    native/wf_web_api.cpp     # lifecycle + provider + last_error + camera-animator
    native/wf_settings.cpp    # background / render mode / lighting / grid / shadows
    native/wf_camera.cpp      # orbital + preset camera
    native/wf_actor.cpp       # spawn / clear + ActorView wrappers
    native/wf_assets.cpp      # AssetManager bridge (needs queue + apply)
    native/web_audio_emitter.cpp
)

set_target_properties(wf_web PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           "wf-core"
    SUFFIX                ".js"  # Emscripten produces wf-core.js + wf-core.wasm
    RUNTIME_OUTPUT_DIRECTORY "${_wf_pkg_dir}"
)

target_link_libraries(wf_web PRIVATE WhiteoutFlakesLib)

# Emscripten link — emdawnwebgpu matches the Dawn webgpu.h the gfx
# backend already targets.
target_link_options(wf_web PRIVATE
    --use-port=emdawnwebgpu
    --pre-js "${CMAKE_CURRENT_SOURCE_DIR}/native/pre.js"
    -sWASM=1
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    # Facade uses wf_last_error() instead of getExceptionMessage.
    -sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','HEAPU8','HEAPF32','stringToUTF8','UTF8ToString','lengthBytesUTF8']
    "-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/native/exports.txt"
    -fno-exceptions
    -Oz   # link-time wasm-opt size pass; TUs compile at -O2.
    -g1   # name section, no DWARF blow-up.
)
target_compile_options(wf_web PRIVATE -fno-exceptions -O2 -g1)

# Stage the package's JS + manifest next to wf-core.{js,wasm} so the
# directory at ${_wf_pkg_dir} is publish-ready.
foreach(_f
    package.json
    index.js
    hive-app.js
    load-table.js
    web-audio.js
    wf-asset-pump.js
    wf-instance.js
    wf-math.js
    wf-prefetch.js
    wf-viewer.js)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${_f}"
                   "${_wf_pkg_dir}/${_f}" COPYONLY)
endforeach()

# Stage the top-level LICENSE into the package — npm publish reads it
# from the publish-tarball root to populate the package's listing page.
configure_file("${CMAKE_SOURCE_DIR}/LICENSE"
               "${_wf_pkg_dir}/LICENSE" COPYONLY)

# Expose the publish-ready dir to parent scope so the demo CMakeLists
# can reference it without re-deriving the path.
set(WF_WEB_PACKAGE_DIR "${_wf_pkg_dir}" PARENT_SCOPE)

# Stage prebuilt WGSL .bls bundles into the package. Only the webgpu/
# subtree ships — d3d12/metal/vulkan/ps/vs are ~80 MB combined and have
# no use in a browser build. wf-prefetch.js resolves shader URLs via
# import.meta.url, so the layout below (./shaders/webgpu/... inside the
# package) is what the JS fetches, and what an `npm install` consumer
# gets without any extra wiring.
if(EXISTS "${CMAKE_SOURCE_DIR}/prebuilt/shaders/webgpu")
    add_custom_command(TARGET wf_web POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/prebuilt/shaders/webgpu"
            "${_wf_pkg_dir}/shaders/webgpu"
        COMMENT "Staging prebuilt WGSL .bls bundles into whiteout-js-viewer/shaders/webgpu"
    )
else()
    message(WARNING "prebuilt/shaders/webgpu missing — wasm build will have no shader bundles to serve")
endif()
