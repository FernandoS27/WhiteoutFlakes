# wf_web — Emscripten/WebGPU browser host. Outputs wf-core.{js,wasm}
# that wf-viewer.js instantiates. Only entered under if(EMSCRIPTEN).

add_executable(wf_web
    wf_web_api.cpp     # lifecycle + provider + last_error + camera-animator
    wf_settings.cpp    # background / render mode / lighting / grid / shadows
    wf_camera.cpp      # orbital + preset camera
    wf_actor.cpp       # spawn / clear + ActorView wrappers
    wf_assets.cpp      # AssetManager bridge (needs queue + apply)
    web_audio_emitter.cpp
)

set_target_properties(wf_web PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           "wf-core"
    SUFFIX                ".js"  # Emscripten produces wf-core.js + wf-core.wasm
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
)

target_link_libraries(wf_web PRIVATE WhiteoutFlakesLib)

# Emscripten link — emdawnwebgpu matches the Dawn webgpu.h the gfx
# backend already targets.
target_link_options(wf_web PRIVATE
    --use-port=emdawnwebgpu
    --pre-js "${CMAKE_CURRENT_SOURCE_DIR}/pre.js"
    -sWASM=1
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    # Facade uses wf_last_error() instead of getExceptionMessage.
    -sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','HEAPU8','HEAPF32','stringToUTF8','UTF8ToString','lengthBytesUTF8']
    "-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/exports.txt"
    -fno-exceptions
    -Oz   # link-time wasm-opt size pass; TUs compile at -O2.
    -g1   # name section, no DWARF blow-up.
)
target_compile_options(wf_web PRIVATE -fno-exceptions -O2 -g1)

# Stage JS/HTML next to wf-core.{js,wasm} for the dev server.
set(_wf_web_files
    wf-viewer.js
    wf-math.js
    wf-instance.js
    wf-asset-pump.js
    wf-prefetch.js
    web_audio.js
    hive_app.js
    index.html
    sw.js
)
foreach(_f IN LISTS _wf_web_files)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${_f}"
                   "${CMAKE_BINARY_DIR}/web/${_f}" COPYONLY)
endforeach()

# Stage prebuilt WGSL .bls bundles — BLS cache queries `shaders/webgpu/{vs,ps}/<name>.bls`.
if(EXISTS "${CMAKE_SOURCE_DIR}/prebuilt/shaders/webgpu")
    add_custom_command(TARGET wf_web POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/prebuilt/shaders"
            "${CMAKE_BINARY_DIR}/web/shaders"
        COMMENT "Staging prebuilt BLS bundles into web/shaders"
    )
else()
    message(WARNING "prebuilt/shaders/webgpu missing — web build will have no shader bundles to serve")
endif()
