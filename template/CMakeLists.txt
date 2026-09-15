cmake_minimum_required(VERSION 3.16)
project(myapp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build with debug info by default; override with -DCMAKE_BUILD_TYPE=Release
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

add_executable(myapp
	src/main.c
)

target_include_directories(myapp PRIVATE include)
target_compile_options(myapp PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# Board libraries. Uncomment the block you need; they resolve against the SDK
# sysroot because environment-setup points PKG_CONFIG_PATH there.
# ---------------------------------------------------------------------------

# --- GStreamer -------------------------------------------------------------
# find_package(PkgConfig REQUIRED)
# pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0)
# target_include_directories(myapp PRIVATE ${GST_INCLUDE_DIRS})
# target_link_libraries(myapp PRIVATE ${GST_LIBRARIES})
# target_compile_options(myapp PRIVATE ${GST_CFLAGS_OTHER})

# --- TensorFlow Lite (no .pc file; link by name) ---------------------------
# target_link_libraries(myapp PRIVATE tensorflowlite_c)

# --- Threads ---------------------------------------------------------------
# find_package(Threads REQUIRED)
# target_link_libraries(myapp PRIVATE Threads::Threads)

install(TARGETS myapp DESTINATION .)
