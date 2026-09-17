#[=======================================================================[.rst:
FindNGX
-------

Find NVIDIA's NGX SDK, which is what DLSS is: a static library, its headers and the signed
feature libraries it loads at runtime. Prebuilt and licensed by NVIDIA rather than by this tree,
so it is found rather than vendored; a checkout of https://github.com/NVIDIA/DLSS is one.

Use this module by invoking find_package with the form::

.. code-block:: cmake

  find_package(NGX
    [version] [EXACT]      # The feature libraries' version, e.g. 310.7.0
    [REQUIRED]             # Fail with error if NGX is not found
  )

Point it at the checkout the way CMake points every package: ``NGX_ROOT``, as a cache entry or in
the environment, or ``CMAKE_PREFIX_PATH``.

Imported targets
^^^^^^^^^^^^^^^^

This module defines the following :prop_tgt:`IMPORTED` targets:

.. variable:: NGX::NGX

  The static library, its headers and the loader it dlopens the features with. The headers are
  system headers, as those of every imported target are, because they are NVIDIA's and a warning in
  them is not ours to fix.

Result variables
^^^^^^^^^^^^^^^^

.. variable:: NGX_FOUND

  Set to true if the SDK was found, otherwise false or undefined.

.. variable:: NGX_VERSION

  The version the feature libraries carry: in their file names on Linux, in their version
  resource on Windows.

.. variable:: NGX_FEATURE_DIR

  The directory holding the release feature libraries, which the application names to NGX at
  runtime.

Cache variables
^^^^^^^^^^^^^^^

For users who wish to edit and control the module behavior, this module
reads hints about search locations from the following variables::

.. variable:: NGX_INCLUDE_DIR

  Path to the include directory with ``nvsdk_ngx.h``.

.. variable:: NGX_LIBRARY

  Path to ``libnvsdk_ngx.a``, or on Windows to ``nvsdk_ngx_d.lib``.

.. variable:: NGX_LIBRARY_DEBUG

  Windows only: path to ``nvsdk_ngx_d_dbg.lib``, the build against the debug runtime.

NOTE: The variables above should not usually be used in CMakeLists.txt files!

#]=======================================================================]

### Find library ##############################################################
# The SDK lays each platform out under its own directory and names the library differently in
# each. On Windows the `_d` suffix is the dynamic C runtime, the one every configuration but Debug
# links, and `_dbg` its debug counterpart: an object compiled against one runtime does not link
# against a library built for the other.
if(WIN32)
    find_library(NGX_LIBRARY NAMES nvsdk_ngx_d PATH_SUFFIXES lib/Windows_x86_64/x64)
    find_library(NGX_LIBRARY_DEBUG NAMES nvsdk_ngx_d_dbg PATH_SUFFIXES lib/Windows_x86_64/x64)
else()
    find_library(NGX_LIBRARY NAMES nvsdk_ngx PATH_SUFFIXES lib/Linux_x86_64)
endif()

### Find include directory ####################################################
find_path(NGX_INCLUDE_DIR NAMES nvsdk_ngx.h PATH_SUFFIXES include)

### Find the feature libraries and their version ##############################
# A little-endian integer of `bytes` bytes at byte `offset` of `hex`, the way `file(READ ... HEX)`
# spells a file.
function(_ngx_read_little_endian hex offset bytes out)
    set(_value "")
    math(EXPR _last "${bytes} - 1")
    foreach(_i RANGE ${_last} 0 -1)
        math(EXPR _at "(${offset} + ${_i}) * 2")
        string(SUBSTRING "${hex}" ${_at} 2 _byte)
        string(APPEND _value "${_byte}")
    endforeach()
    math(EXPR _value "0x${_value}" OUTPUT_FORMAT DECIMAL)
    set(${out} "${_value}" PARENT_SCOPE)
endfunction()

# The `major.minor.build` a Windows DLL states in its version resource, or empty where it has
# none. The resource is `VS_FIXEDFILEINFO`: its signature `0xFEEF04BD` and structure version
# `0x00010000`, then the file version as two little-endian doublewords. Only the `.rsrc` section
# is read — the DOS header names the PE header, whose section table says where that section sits
# in the file — because the signature's bytes also occur in the code that checks for it, and the
# DLL is sixty megabytes. Each header is checked by its own signature before anything is read
# through it, and a file whose headers lie beyond the first four kilobytes is not one this reads.
function(_ngx_read_dll_version dll out)
    set(${out} "" PARENT_SCOPE)

    set(_limit 4096)
    file(READ "${dll}" _head LIMIT ${_limit} HEX)
    string(SUBSTRING "${_head}" 0 4 _mz)
    if(NOT _mz STREQUAL "4d5a") # "MZ"
        return()
    endif()

    _ngx_read_little_endian("${_head}" 60 4 _pe)
    math(EXPR _end "${_pe} + 24")
    if(_end GREATER _limit)
        return()
    endif()
    math(EXPR _at "${_pe} * 2")
    string(SUBSTRING "${_head}" ${_at} 8 _signature)
    if(NOT _signature STREQUAL "50450000") # "PE\0\0"
        return()
    endif()

    math(EXPR _at "${_pe} + 6")
    _ngx_read_little_endian("${_head}" ${_at} 2 _sections)
    math(EXPR _at "${_pe} + 20")
    _ngx_read_little_endian("${_head}" ${_at} 2 _optional)
    math(EXPR _table "${_pe} + 24 + ${_optional}")
    math(EXPR _end "${_table} + ${_sections} * 40")
    if(_sections EQUAL 0 OR _end GREATER _limit)
        return()
    endif()

    math(EXPR _last "${_sections} - 1")
    foreach(_i RANGE 0 ${_last})
        math(EXPR _header "${_table} + ${_i} * 40")
        math(EXPR _at "${_header} * 2")
        string(SUBSTRING "${_head}" ${_at} 16 _name)
        if(NOT _name STREQUAL "2e72737263000000") # ".rsrc", padded to eight
            continue()
        endif()

        math(EXPR _at "${_header} + 16")
        _ngx_read_little_endian("${_head}" ${_at} 4 _size)
        math(EXPR _at "${_header} + 20")
        _ngx_read_little_endian("${_head}" ${_at} 4 _offset)
        file(READ "${dll}" _rsrc OFFSET ${_offset} LIMIT ${_size} HEX)

        string(FIND "${_rsrc}" "bd04effe00000100" _fixed)
        math(EXPR _odd "${_fixed} % 2") # a hit between two bytes is not the structure
        if(_fixed LESS 0 OR _odd)
            return()
        endif()

        math(EXPR _at "${_fixed} / 2 + 8")
        _ngx_read_little_endian("${_rsrc}" ${_at} 4 _high)
        math(EXPR _at "${_fixed} / 2 + 12")
        _ngx_read_little_endian("${_rsrc}" ${_at} 4 _low)
        math(EXPR _major "${_high} >> 16")
        math(EXPR _minor "${_high} & 0xFFFF")
        math(EXPR _build "${_low} >> 16")
        set(${out} "${_major}.${_minor}.${_build}" PARENT_SCOPE)
        return()
    endforeach()
endfunction()

# The release features sit beside the static library — one directory up on Windows, where the
# import libraries have a directory of their own — and each states the SDK's version: the one
# place a checkout says which tag it is. Linux carries it in the file name; Windows carries it in
# the DLL's version resource.
if(NGX_LIBRARY)
    get_filename_component(_ngx_library_dir "${NGX_LIBRARY}" DIRECTORY)
    if(WIN32)
        set(_ngx_dlss_feature "${_ngx_library_dir}/../rel/nvngx_dlss.dll")
        if(EXISTS "${_ngx_dlss_feature}")
            get_filename_component(NGX_FEATURE_DIR "${_ngx_library_dir}/../rel" ABSOLUTE)
            _ngx_read_dll_version("${_ngx_dlss_feature}" NGX_VERSION)
        endif()
    else()
        file(GLOB _ngx_dlss_feature "${_ngx_library_dir}/rel/libnvidia-ngx-dlss.so.*")
        if(_ngx_dlss_feature)
            set(NGX_FEATURE_DIR "${_ngx_library_dir}/rel")
            string(REGEX MATCH "\\.so\\.([0-9]+\\.[0-9]+\\.[0-9]+)$" _ngx_version_match "${_ngx_dlss_feature}")
            set(NGX_VERSION "${CMAKE_MATCH_1}")
        endif()
        unset(_ngx_version_match)
    endif()
    unset(_ngx_library_dir)
    unset(_ngx_dlss_feature)
endif()

### Set result variables ######################################################
# The tag to clone is the version asked for, where one was.
if(NGX_FIND_VERSION)
    set(_ngx_branch "--branch v${NGX_FIND_VERSION} ")
else()
    set(_ngx_branch "")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NGX
        REQUIRED_VARS NGX_LIBRARY NGX_INCLUDE_DIR NGX_FEATURE_DIR
        VERSION_VAR NGX_VERSION
        REASON_FAILURE_MESSAGE "Clone it with `git clone --depth 1 ${_ngx_branch}https://github.com/NVIDIA/DLSS.git` and point NGX_ROOT at the checkout.")
unset(_ngx_branch)

set(NGX_INCLUDE_DIR ${NGX_INCLUDE_DIR} CACHE PATH "NGX include dir hint")
set(NGX_LIBRARY ${NGX_LIBRARY} CACHE FILEPATH "NGX library path hint")
mark_as_advanced(NGX_INCLUDE_DIR NGX_LIBRARY NGX_LIBRARY_DEBUG)

### Import targets ############################################################
if(NGX_FOUND)
    if(NOT TARGET NGX::NGX)
        add_library(NGX::NGX STATIC IMPORTED)
        set_target_properties(NGX::NGX PROPERTIES
                IMPORTED_LOCATION "${NGX_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${NGX_INCLUDE_DIR}"
                INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS}")
        if(NGX_LIBRARY_DEBUG)
            set_target_properties(NGX::NGX PROPERTIES
                    IMPORTED_LOCATION_DEBUG "${NGX_LIBRARY_DEBUG}")
        endif()
    endif()
endif()
