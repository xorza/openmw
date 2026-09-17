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
# The `major.minor.build` a Windows DLL states in its version resource, or empty where it has
# none. Read by the system's own reader, because CMake has no command for the format and the
# feature DLLs carry no version in their names the way the Linux libraries do: one start of
# PowerShell per configure, which is the price of not parsing a PE file by hand. `FileVersionInfo`
# is .NET's, so Windows PowerShell and PowerShell 7 answer alike.
function(_ngx_read_dll_version dll out)
    string(REPLACE "'" "''" _quoted "${dll}")
    execute_process(
        COMMAND powershell -NoProfile -NonInteractive -Command
            "$v = [System.Diagnostics.FileVersionInfo]::GetVersionInfo('${_quoted}'); '{0}.{1}.{2}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_status EQUAL 0)
        set(${out} "${_version}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
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
# Two configurations and a map from the other two, the way NVIDIA's own module (nvpro_core2's
# `FindNGX.cmake`) imports it: `RelWithDebInfo` and `MinSizeRel` link the release runtime's
# library, and only `Debug` links the debug runtime's. Linux has one library for both.
if(NGX_FOUND)
    if(NOT TARGET NGX::NGX)
        add_library(NGX::NGX STATIC IMPORTED)
        set_target_properties(NGX::NGX PROPERTIES
                IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
                IMPORTED_LOCATION_RELEASE "${NGX_LIBRARY}"
                MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
                MAP_IMPORTED_CONFIG_MINSIZEREL Release
                INTERFACE_INCLUDE_DIRECTORIES "${NGX_INCLUDE_DIR}"
                INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS}")
        if(NGX_LIBRARY_DEBUG)
            set_target_properties(NGX::NGX PROPERTIES IMPORTED_LOCATION_DEBUG "${NGX_LIBRARY_DEBUG}")
        else()
            set_target_properties(NGX::NGX PROPERTIES IMPORTED_LOCATION_DEBUG "${NGX_LIBRARY}")
        endif()
    endif()
endif()
