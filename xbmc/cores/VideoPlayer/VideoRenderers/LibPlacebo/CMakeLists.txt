if(NOT TARGET ${APP_NAME_LC}::Libplacebo)
  return()
endif()

set(SOURCES libav_wrapper.c
            PlHelper.cpp)

set(HEADERS
            PlHelper.h)

if(NOT (WIN32 OR WINDOWS_STORE))
  list(APPEND HEADERS RendererPLBase.h)
  list(APPEND SOURCES RendererPLGL.cpp)
  list(APPEND HEADERS RendererPLGL.h)
  if(TARGET ${APP_NAME_LC}::OpenGLES)
    list(APPEND SOURCES RendererPLGLES.cpp)
    list(APPEND HEADERS RendererPLGLES.h)
  endif()
endif()

core_add_library(videorenderers-libplacebo)

if(TARGET ${APP_NAME_LC}::VAAPI)
  target_link_libraries(videorenderers-libplacebo PRIVATE ${APP_NAME_LC}::VAAPI)
endif()
