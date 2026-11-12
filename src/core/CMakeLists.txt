# OBS-independent core: VDO.Ninja protocol, signaling, WebRTC sessions and RTP handling.
#
# Nothing in here may include libobs or Qt headers, which keeps the networking code unit-testable without OBS and
# makes the service layer replaceable (see session/guest-service.hpp).

include(FetchContent)

add_library(quick-cast-core STATIC)

target_sources(
  quick-cast-core
  PRIVATE
    rtp/receive-stream.cpp
    rtp/receive-stream.hpp
    rtp/reorder-buffer.cpp
    rtp/reorder-buffer.hpp
    rtp/rtcp.cpp
    rtp/rtcp.hpp
    rtp/rtp-packet.cpp
    rtp/rtp-packet.hpp
    rtp/sync-clock.cpp
    rtp/sync-clock.hpp
    rtp/video-depacketizer.cpp
    rtp/video-depacketizer.hpp
    session/guest-service.hpp
    util/crypto.cpp
    util/crypto.hpp
    util/executor.cpp
    util/executor.hpp
    util/log.cpp
    util/log.hpp
    vdo/data-messages.cpp
    vdo/data-messages.hpp
    vdo/identity.cpp
    vdo/identity.hpp
    vdo/links.cpp
    vdo/links.hpp
    vdo/media-receiver.cpp
    vdo/media-receiver.hpp
    vdo/messages.cpp
    vdo/messages.hpp
    vdo/sdp-utils.cpp
    vdo/sdp-utils.hpp
    vdo/signaling-client.cpp
    vdo/signaling-client.hpp
    vdo/vdo-service.cpp
    vdo/vdo-service.hpp
)

target_include_directories(quick-cast-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(quick-cast-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(
  quick-cast-core
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE LibDataChannel::LibDataChannelStatic Threads::Threads
)

if(QUICK_CAST_TLS_BACKEND STREQUAL "mbedtls")
  # Reuse the finder libdatachannel ships so both agree on the same Mbed TLS.
  FetchContent_GetProperties(libdatachannel SOURCE_DIR _libdatachannel_source_dir)
  list(APPEND CMAKE_MODULE_PATH "${_libdatachannel_source_dir}/cmake/Modules")
  find_package(MbedTLS REQUIRED)
  target_compile_definitions(quick-cast-core PRIVATE QC_TLS_MBEDTLS)
  target_link_libraries(quick-cast-core PRIVATE MbedTLS::MbedTLS)
else()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(quick-cast-core PRIVATE OpenSSL::Crypto)
endif()
