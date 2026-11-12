#[=======================================================================[.rst:
FindQCFFmpeg
------------

Locates the FFmpeg libraries Quick Cast decodes with (libavcodec, libavutil).

On Windows and macOS these come from the pre-built obs-deps package (already on ``CMAKE_PREFIX_PATH``), on Linux from
the system via pkg-config. OBS ships the matching runtime libraries, so nothing has to be bundled with the plugin.

Imported targets: ``QCFFmpeg::avcodec``, ``QCFFmpeg::avutil``
#]=======================================================================]

include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)

set(_qcffmpeg_required_vars "")
foreach(_component IN ITEMS avcodec avutil)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_QCFFmpeg_${_component} QUIET lib${_component})
  endif()

  find_path(
    QCFFmpeg_${_component}_INCLUDE_DIR
    NAMES lib${_component}/${_component}.h
    HINTS ${PC_QCFFmpeg_${_component}_INCLUDE_DIRS}
    PATH_SUFFIXES ffmpeg
  )
  find_library(
    QCFFmpeg_${_component}_LIBRARY
    NAMES ${_component} lib${_component}
    HINTS ${PC_QCFFmpeg_${_component}_LIBRARY_DIRS}
  )
  list(APPEND _qcffmpeg_required_vars QCFFmpeg_${_component}_LIBRARY QCFFmpeg_${_component}_INCLUDE_DIR)
  mark_as_advanced(QCFFmpeg_${_component}_INCLUDE_DIR QCFFmpeg_${_component}_LIBRARY)
endforeach()

find_package_handle_standard_args(QCFFmpeg REQUIRED_VARS ${_qcffmpeg_required_vars})

if(QCFFmpeg_FOUND)
  foreach(_component IN ITEMS avcodec avutil)
    if(NOT TARGET QCFFmpeg::${_component})
      add_library(QCFFmpeg::${_component} UNKNOWN IMPORTED)
      set_target_properties(
        QCFFmpeg::${_component}
        PROPERTIES
          IMPORTED_LOCATION "${QCFFmpeg_${_component}_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${QCFFmpeg_${_component}_INCLUDE_DIR}"
      )
    endif()
  endforeach()
endif()
unset(_qcffmpeg_required_vars)
