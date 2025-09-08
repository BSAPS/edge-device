cmake_minimum_required(VERSION 3.16)
project(BsapsEdgeDevice CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find libcamera using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

add_executable(bsaps_app main.cpp)

# Include directories for bsaps_app
target_include_directories(bsaps_app PRIVATE ${LIBCAMERA_INCLUDE_DIRS})

# Link libraries for bsaps_app
target_link_libraries(bsaps_app PRIVATE
    ${LIBCAMERA_LIBRARIES}
    pthread
)

install(TARGETS bsaps_app DESTINATION bin)
