# Run in Crashpad's source directory after it is extracted.
#
# **Its socket transport on Linux, where it asks for libcurl.** The transport exists to upload a
# report, and nothing here uploads; libcurl would be a library every build and the AppImage carry
# for no call. The socket transport is what Crashpad builds for Android, with nothing to find. A
# release of Crashpad that words the block differently stops the configure here, by name, rather
# than building with libcurl or without a transport.
set(file util/CMakeLists.txt)
file(READ ${file} text)

set(curl [[        if(NOT TARGET CURL::libcurl) # Some other lib might bring libcurl already
            find_package(CURL REQUIRED)
        endif()

        target_link_libraries(crashpad_util PRIVATE CURL::libcurl)
        SET(HTTP_TRANSPORT_IMPL net/http_transport_libcurl.cc)]])

string(FIND "${text}" "${curl}" at)
if (at EQUAL -1)
    message(FATAL_ERROR "crashpadpatch.cmake: ${file} no longer asks for libcurl in the words it replaces")
endif()

string(REPLACE "${curl}" "        SET(HTTP_TRANSPORT_IMPL net/http_transport_socket.cc)" text "${text}")
file(WRITE ${file} "${text}")
