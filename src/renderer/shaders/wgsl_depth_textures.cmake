# ============================================================================
# wgsl_depth_textures.cmake — retype comparison-sampled textures in a slangc
# WGSL output.
#
# slangc declares a texture sampled through textureSampleCompareLevel as
# texture_*<f32>, which Dawn rejects. Wc3Shaders' own build patches the same
# thing (compile_all_slang.py fix_wgsl_depth_textures); the engine entries that
# import wc3_shaders run this instead. Globals only: every comparison sample in
# those entries names its texture directly.
#
# Input (via -D): WGSL — the file to patch in place.
# ============================================================================
cmake_minimum_required(VERSION 3.16)

file(READ "${WGSL}" text)
string(REGEX MATCHALL "textureSampleCompareLevel\\(\\(?[A-Za-z_][A-Za-z0-9_]*" calls "${text}")
set(names "")
foreach(call IN LISTS calls)
    string(REGEX REPLACE "^textureSampleCompareLevel\\(\\(?" "" name "${call}")
    list(APPEND names "${name}")
endforeach()
if(names)
    list(REMOVE_DUPLICATES names)
endif()
foreach(name IN LISTS names)
    string(REPLACE "${name} : texture_2d_array<f32>" "${name} : texture_depth_2d_array"
           text "${text}")
    string(REPLACE "${name} : texture_cube_array<f32>" "${name} : texture_depth_cube_array"
           text "${text}")
    string(REPLACE "${name} : texture_2d<f32>" "${name} : texture_depth_2d" text "${text}")
    string(REPLACE "${name} : texture_cube<f32>" "${name} : texture_depth_cube" text "${text}")
endforeach()
file(WRITE "${WGSL}" "${text}")
