# cmake -DSRC=<dir> -DDST=<dir> [-DSTAMP=<file>] -P copy_dir_unless_same.cmake
#
# Copy SRC's contents into DST unless both name the same directory, then touch
# STAMP. The shader pack stages into standalone/, which is the exe directory
# for some generators and configs and a parent of it for others; this copies
# into the exe directory only where the two differ. file(COPY) skips files
# whose timestamps already match, so a restage costs only what changed.

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "copy_dir_unless_same.cmake needs -DSRC and -DDST")
endif()

get_filename_component(_src "${SRC}" REALPATH)
get_filename_component(_dst "${DST}" REALPATH)
if(NOT _src STREQUAL _dst AND EXISTS "${_src}")
    file(MAKE_DIRECTORY "${_dst}")
    file(COPY "${_src}/" DESTINATION "${_dst}")
endif()

if(DEFINED STAMP)
    file(TOUCH "${STAMP}")
endif()
