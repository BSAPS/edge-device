cmake_minimum_required(VERSION 3.16)
project(BsapsEdgeDevice CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

# Find libcamera using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

# Find GStreamer using pkg-config
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0)

add_executable(bsaps_app main.cpp)

# Include directories for bsaps_app
target_include_directories(bsaps_app PRIVATE 
    ${LIBCAMERA_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
)

# Link libraries for bsaps_app
target_link_libraries(bsaps_app PRIVATE
    ${LIBCAMERA_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
    pthread
)

install(TARGETS bsaps_app DESTINATION bin)
