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

  The version the feature libraries carry in their file names.

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

  Path to ``libnvsdk_ngx.a``.

NOTE: The variables above should not usually be used in CMakeLists.txt files!

#]=======================================================================]

### Find library ##############################################################
find_library(NGX_LIBRARY NAMES nvsdk_ngx PATH_SUFFIXES lib/Linux_x86_64)

### Find include directory ####################################################
find_path(NGX_INCLUDE_DIR NAMES nvsdk_ngx.h PATH_SUFFIXES include)

### Find the feature libraries and their version ##############################
# The release features sit beside the static library, and each carries the SDK's version in its
# name: the one place a checkout states which tag it is.
if(NGX_LIBRARY)
    get_filename_component(_ngx_library_dir "${NGX_LIBRARY}" DIRECTORY)
    file(GLOB _ngx_dlss_feature "${_ngx_library_dir}/rel/libnvidia-ngx-dlss.so.*")
    if(_ngx_dlss_feature)
        set(NGX_FEATURE_DIR "${_ngx_library_dir}/rel")
        string(REGEX MATCH "\\.so\\.([0-9]+\\.[0-9]+\\.[0-9]+)$" _ngx_version_match "${_ngx_dlss_feature}")
        set(NGX_VERSION "${CMAKE_MATCH_1}")
    endif()
    unset(_ngx_library_dir)
    unset(_ngx_dlss_feature)
    unset(_ngx_version_match)
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
mark_as_advanced(NGX_INCLUDE_DIR NGX_LIBRARY)

### Import targets ############################################################
if(NGX_FOUND)
    if(NOT TARGET NGX::NGX)
        add_library(NGX::NGX STATIC IMPORTED)
        set_target_properties(NGX::NGX PROPERTIES
                IMPORTED_LOCATION "${NGX_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${NGX_INCLUDE_DIR}"
                INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS}")
    endif()
endif()
