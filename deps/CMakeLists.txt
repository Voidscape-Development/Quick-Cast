# Third-party dependencies.
#
# Everything here is built as a static library and linked into the plugin module so the shipped plugin is a single
# self-contained binary that does not depend on (or conflict with) whatever libdatachannel build OBS itself ships.

include(FetchContent)

# Third-party code is not held to this project's warning policy.
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
if(MSVC)
  add_compile_options(/w)
else()
  add_compile_options(-w)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)

# --- nlohmann_json -------------------------------------------------------------------------------------------------
find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- libdatachannel ------------------------------------------------------------------------------------------------
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_JSON ON CACHE BOOL "" FORCE)
# libSRTP: no test programs, and do not promote its warnings to errors on compilers we do not control.
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_WARNINGS OFF CACHE BOOL "" FORCE)

if(WIN32 OR APPLE)
  # obs-deps ships Mbed TLS but not OpenSSL.
  set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
  set(QUICK_CAST_TLS_BACKEND "mbedtls" CACHE INTERNAL "")
else()
  set(QUICK_CAST_TLS_BACKEND "openssl" CACHE INTERNAL "")
endif()

FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.23.2
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(libdatachannel)
