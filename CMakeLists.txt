# This file is automatically generated from cmake.toml - DO NOT EDIT
# See https://github.com/build-cpp/cmkr for more information

cmake_minimum_required(VERSION 3.15)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
	message(FATAL_ERROR "In-tree builds are not supported. Run CMake from a separate directory: cmake -B build")
endif()

set(CMKR_ROOT_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	set(CMKR_ROOT_PROJECT ON)

	# Bootstrap cmkr and automatically regenerate CMakeLists.txt
	include(cmkr.cmake OPTIONAL RESULT_VARIABLE CMKR_INCLUDE_RESULT)
	if(CMKR_INCLUDE_RESULT)
		cmkr()
	endif()

	# Enable folder support
	set_property(GLOBAL PROPERTY USE_FOLDERS ON)

	# Create a configure-time dependency on cmake.toml to improve IDE support
	configure_file(cmake.toml cmake.toml COPYONLY)
endif()

 
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)

project(ff7r-proj)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")

set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)

if ("${CMAKE_BUILD_TYPE}" MATCHES "Release")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MT")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MT")

    # Statically compile runtime
    string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")

    message(NOTICE "Building in Release mode")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# Fix warnings about DOWNLOAD_EXTRACT_TIMESTAMP
if(POLICY CMP0135)
	cmake_policy(SET CMP0135 NEW)
endif()
message(STATUS "Fetching bddisasm (70db095765ab2066dd88dfb7bbcc42259ed167c5)...")
FetchContent_Declare(bddisasm SYSTEM
	GIT_REPOSITORY
		"https://github.com/bitdefender/bddisasm"
	GIT_TAG
		70db095765ab2066dd88dfb7bbcc42259ed167c5
)
FetchContent_MakeAvailable(bddisasm)

message(STATUS "Fetching spdlog (76fb40d95455f249bd70824ecfcae7a8f0930fa3)...")
FetchContent_Declare(spdlog SYSTEM
	GIT_REPOSITORY
		"https://github.com/gabime/spdlog"
	GIT_TAG
		76fb40d95455f249bd70824ecfcae7a8f0930fa3
)
FetchContent_MakeAvailable(spdlog)

message(STATUS "Fetching kananlib (7ceca3f80a1ec4f89b613afb3f5b41832766609e)...")
FetchContent_Declare(kananlib SYSTEM
	GIT_REPOSITORY
		"https://github.com/cursey/kananlib"
	GIT_TAG
		7ceca3f80a1ec4f89b613afb3f5b41832766609e
)
FetchContent_MakeAvailable(kananlib)

# Target: ff7rebirth_
set(ff7rebirth__SOURCES
	"src/Plugin.cpp"
	"src/uevr/API.hpp"
	"src/uevr/Plugin.hpp"
	"src/uevr/API.h"
	cmake.toml
)

add_library(ff7rebirth_ SHARED)

target_sources(ff7rebirth_ PRIVATE ${ff7rebirth__SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ff7rebirth__SOURCES})

target_compile_features(ff7rebirth_ PUBLIC
	cxx_std_20
)

target_compile_options(ff7rebirth_ PUBLIC
	"/GS-"
	"/bigobj"
	"/EHa"
	"/MP"
)

target_include_directories(ff7rebirth_ PUBLIC
	"src/"
)

target_link_libraries(ff7rebirth_ PUBLIC
	kananlib
)

set(CMKR_TARGET ff7rebirth_)
target_compile_definitions(ff7rebirth_ PUBLIC 
    NOMINMAX
    WINVER=0x0A00
)
