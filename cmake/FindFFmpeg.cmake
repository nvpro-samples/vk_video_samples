#
# Find the native FFMPEG includes and library
# This module defines
# FFMPEG_INCLUDE_DIR, where to find avcodec.h, avformat.h ...
# FFMPEG_LIBRARIES, the libraries to link against to use FFMPEG.
# FFMPEG_FOUND, If false, do not try to use FFMPEG.
# FFMPEG_ROOT, if this module use this path to find FFMPEG headers
# and libraries.

# Macro to find header and lib directories
# example: FFMPEG_FIND(AVFORMAT avformat avformat.h)
MACRO(FFMPEG_FIND varname shortname headername)
    # old version of ffmpeg put header in $prefix/include/[ffmpeg]
    # so try to find header in include directory
    FIND_PATH(FFMPEG_${varname}_INCLUDE_DIRS lib${shortname}/${headername}
        PATHS
        ${FFMPEG_ROOT}/include/lib${shortname}
        $ENV{FFMPEG_DIR}/include/lib${shortname}
        ~/Library/Frameworks/lib${shortname}
        /Library/Frameworks/lib${shortname}
        /usr/local/include/lib${shortname}
        /usr/include/lib${shortname}
        /sw/include/lib${shortname} # Fink
        /opt/local/include/lib${shortname} # DarwinPorts
        /opt/csw/include/lib${shortname} # Blastwave
        /opt/include/lib${shortname}
        /usr/freeware/include/lib${shortname}
        PATH_SUFFIXES ffmpeg
        DOC "Location of FFMPEG Headers"
    )

    FIND_PATH(FFMPEG_${varname}_INCLUDE_DIRS lib${shortname}/${headername}
        PATHS
        ${FFMPEG_ROOT}/include
        $ENV{FFMPEG_DIR}/include
        ~/Library/Frameworks
        /Library/Frameworks
        /usr/local/include
        /usr/include
        /sw/include # Fink
        /opt/local/include # DarwinPorts
        /opt/csw/include # Blastwave
        /opt/include
        /usr/freeware/include
        PATH_SUFFIXES ffmpeg
        DOC "Location of FFMPEG Headers"
    )

    FIND_LIBRARY(FFMPEG_${varname}_LIBRARIES
        NAMES ${shortname}
        PATHS
        ${FFMPEG_ROOT}/lib
        $ENV{FFMPEG_DIR}/lib
        ~/Library/Frameworks
        /Library/Frameworks
        /usr/local/lib
        /usr/local/lib64
        /usr/lib/x86_64-linux-gnu
        /usr/lib
        /usr/lib64
        /sw/lib
        /opt/local/lib
        /opt/csw/lib
        /opt/lib
        /usr/freeware/lib64
        DOC "Location of FFMPEG Libraries"
    )

    IF (FFMPEG_${varname}_LIBRARIES AND FFMPEG_${varname}_INCLUDE_DIRS)
        SET(FFMPEG_${varname}_FOUND 1)
    ENDIF(FFMPEG_${varname}_LIBRARIES AND FFMPEG_${varname}_INCLUDE_DIRS)

ENDMACRO(FFMPEG_FIND)

SET(FFMPEG_ROOT "$ENV{FFMPEG_DIR}" CACHE PATH "Location of FFMPEG")

# find stdint.h
IF(WIN32)

    FIND_PATH(FFMPEG_STDINT_INCLUDE_DIR stdint.h
        PATHS
        ${FFMPEG_ROOT}/include
        $ENV{FFMPEG_DIR}/include
        ~/Library/Frameworks
        /Library/Frameworks
        /usr/local/include
        /usr/include
        /sw/include # Fink
        /opt/local/include # DarwinPorts
        /opt/csw/include # Blastwave
        /opt/include
        /usr/freeware/include
        PATH_SUFFIXES ffmpeg
        DOC "Location of FFMPEG stdint.h Header"
    )

    IF (FFMPEG_STDINT_INCLUDE_DIR)
        SET(STDINT_OK TRUE)
    ENDIF()

ELSE()

    SET(STDINT_OK TRUE)

ENDIF()

FFMPEG_FIND(LIBAVFORMAT avformat avformat.h)
FFMPEG_FIND(LIBAVDEVICE avdevice avdevice.h)
FFMPEG_FIND(LIBAVCODEC  avcodec  avcodec.h)
FFMPEG_FIND(LIBAVUTIL   avutil   avutil.h)
FFMPEG_FIND(LIBSWSCALE  swscale  swscale.h)  # not sure about the header to look for here.
FFMPEG_FIND(LIBX264  x264 x264.h)
FFMPEG_FIND(LIBX265  x265 x265.h)

SET(FFMPEG_FOUND "NO")

# Note we don't check FFMPEG_LIBSWSCALE_FOUND, FFMPEG_LIBAVDEVICE_FOUND,
# and FFMPEG_LIBAVUTIL_FOUND as they are optional.
IF (FFMPEG_LIBAVFORMAT_FOUND AND FFMPEG_LIBAVCODEC_FOUND AND STDINT_OK)

    SET(FFMPEG_FOUND "YES")

    SET(FFMPEG_INCLUDE_DIR ${FFMPEG_LIBAVFORMAT_INCLUDE_DIRS})

    SET(FFMPEG_LIBRARY_DIRS ${FFMPEG_LIBAVFORMAT_LIBRARY_DIRS})

    # Note we don't add FFMPEG_LIBSWSCALE_LIBRARIES here,
    # it will be added if found later.
    SET(FFMPEG_LIBRARIES
        ${FFMPEG_LIBAVFORMAT_LIBRARIES}
        ${FFMPEG_LIBAVDEVICE_LIBRARIES}
        ${FFMPEG_LIBAVCODEC_LIBRARIES}
        ${FFMPEG_LIBAVUTIL_LIBRARIES}
        ${FFMPEG_LIBSWSCALE_LIBRARIES}
        ${FFMPEG_LIBX264_LIBRARIES}
        ${FFMPEG_LIBX265_LIBRARIES})
ENDIF()

# Find FFmpeg components
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FFMPEG QUIET
        libavcodec
        libavformat
        libavutil
        libswscale
    )
endif()

# Find individual components
find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h libavformat/avformat.h libavutil/avutil.h libswscale/swscale.h
    PATHS
        ${FFMPEG_INCLUDE_DIRS}
        /usr/include
        /usr/local/include
        $ENV{FFMPEG_ROOT}/include
    PATH_SUFFIXES ffmpeg
)

# Find libraries
find_library(AVCODEC_LIBRARY
    NAMES avcodec
    PATHS
        ${FFMPEG_LIBRARY_DIRS}
        /usr/lib
        /usr/local/lib
        $ENV{FFMPEG_ROOT}/lib
)

find_library(AVFORMAT_LIBRARY
    NAMES avformat
    PATHS
        ${FFMPEG_LIBRARY_DIRS}
        /usr/lib
        /usr/local/lib
        $ENV{FFMPEG_ROOT}/lib
)

find_library(AVUTIL_LIBRARY
    NAMES avutil
    PATHS
        ${FFMPEG_LIBRARY_DIRS}
        /usr/lib
        /usr/local/lib
        $ENV{FFMPEG_ROOT}/lib
)

find_library(SWSCALE_LIBRARY
    NAMES swscale
    PATHS
        ${FFMPEG_LIBRARY_DIRS}
        /usr/lib
        /usr/local/lib
        $ENV{FFMPEG_ROOT}/lib
)

# Set FFmpeg libraries
set(FFMPEG_LIBRARIES
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
)

# Handle the QUIETLY and REQUIRED arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS
        FFMPEG_INCLUDE_DIR
        AVCODEC_LIBRARY
        AVFORMAT_LIBRARY
        AVUTIL_LIBRARY
        SWSCALE_LIBRARY
)

# Mark as advanced
mark_as_advanced(
    FFMPEG_INCLUDE_DIR
    AVCODEC_LIBRARY
    AVFORMAT_LIBRARY
    AVUTIL_LIBRARY
    SWSCALE_LIBRARY
)

# Set variables for use in the project
if(FFMPEG_FOUND)
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIR})
    set(FFMPEG_DEFINITIONS ${FFMPEG_CFLAGS_OTHER})
endif()
