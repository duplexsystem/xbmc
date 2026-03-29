if(NOT TARGET ${APP_NAME_LC}::Libplacebo)
  return()
endif()

set(SOURCES libav_wrapper.c
        PLHelper.cpp)

set(HEADERS
        PLHelper.h)

if(NOT (WIN32 OR WINDOWS_STORE))
  list(APPEND HEADERS LinuxRendererPLBase.h)
  list(APPEND SOURCES LinuxRendererPLGL.cpp)
  list(APPEND HEADERS LinuxRendererPLGL.h)
  if(TARGET ${APP_NAME_LC}::OpenGLES)
    list(APPEND SOURCES LinuxRendererPLGLES.cpp)
    list(APPEND HEADERS LinuxRendererPLGLES.h)
  endif()
endif()

core_add_library(videorenderers-libplacebo)

if(TARGET ${APP_NAME_LC}::VAAPI)
  target_link_libraries(videorenderers-libplacebo PRIVATE ${APP_NAME_LC}::VAAPI)
endif()
