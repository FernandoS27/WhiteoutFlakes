# Snowball is the re-implementation; Domino is the shipped engine it reproduces. The two names
# are kept apart deliberately — see Domino's DOMINO_RE_PLAN.md section 4.1.
#
# Vendored: this is a copy of the Snowball sources, not a checkout of the sibling repository.
# It stays self-contained — nothing here may reach outside this directory, so the module can be
# resynced by replacing include/ and src/ wholesale.

# Listed explicitly, never globbed: a .cpp that is not named here simply does not build, which
# is the same rule (and the same trap) as the parent project's CMakeLists.
set(SNOWBALL_SOURCES
    src/aabb_tree.cpp
    src/capsule.cpp
    src/cloth.cpp
    src/cloth_authoring.cpp
    src/contact.cpp
    src/distance.cpp
    src/heightfield.cpp
    src/hull.cpp
    src/joint.cpp
    src/manifold.cpp
    src/math.cpp
    src/mesh_contact.cpp
    src/partial_polytope.cpp
    src/polytope.cpp
    src/raycast.cpp
    src/scene.cpp
    src/shape.cpp
    src/solver.cpp
    src/sphere.cpp
    src/toi.cpp
    src/transform.cpp
    src/tree_mesh.cpp
)

add_library(snowball STATIC ${SNOWBALL_SOURCES})
target_include_directories(snowball PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# C++20, matching WhiteoutFlakes, so the public headers can be included by it directly.
target_compile_features(snowball PUBLIC cxx_std_20)

# Domino is built on SSE approximate-reciprocal instructions and their results are part of
# the contract (DOMINO_SPEC.md section 4.1). Fast-math would let the compiler rewrite
# rcpps+Newton-Raphson into a true division — which is precisely the behaviour being
# reproduced and must not be optimised away.
if(MSVC)
    target_compile_options(snowball PRIVATE /W4 /fp:precise)
else()
    target_compile_options(snowball PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
