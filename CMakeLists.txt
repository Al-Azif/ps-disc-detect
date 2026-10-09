cmake_minimum_required(VERSION 3.10.2)

project(disc-detect C CXX)

if(ORBIS AND PROSPERO)
  message(FATAL_ERROR "ORBIS and PROSPERO cannot both be set. Please specify only one target platform.")
endif()

if(ORBIS)
  if(NOT DEFINED ENV{PS4_PAYLOAD_SDK})
    message(FATAL_ERROR "ORBIS build requires the PS4_PAYLOAD_SDK environment variable to be set.")
  endif()
  if(NOT EXISTS "$ENV{PS4_PAYLOAD_SDK}")
    message(FATAL_ERROR "PS4_PAYLOAD_SDK path does not exist: $ENV{PS4_PAYLOAD_SDK}")
  endif()
  add_compile_definitions(__ORBIS__)
  set(PAYLOAD_NAME "disc-ps4.elf")
  set(DISC_BUILD_REL "build/ps4")
  set(DISC_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/ps4")
  message(STATUS "Platform Type: Orbis (PlayStation 4)")
elseif(PROSPERO)
  if(NOT DEFINED ENV{PS5_PAYLOAD_SDK})
    message(FATAL_ERROR "PROSPERO build requires the PS5_PAYLOAD_SDK environment variable to be set.")
  endif()
  if(NOT EXISTS "$ENV{PS5_PAYLOAD_SDK}")
    message(FATAL_ERROR "PS5_PAYLOAD_SDK path does not exist: $ENV{PS5_PAYLOAD_SDK}")
  endif()
  add_compile_definitions(__PROSPERO__)
  set(PAYLOAD_NAME "disc-ps5.elf")
  set(DISC_BUILD_REL "build/ps5")
  set(DISC_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/ps5")
  message(STATUS "Platform Type: Prospero (PlayStation 5)")
else()
  message(FATAL_ERROR "Configure with orbis-cmake or prospero-cmake.")
endif()

get_filename_component(DISC_BUILD_DIR "${DISC_BUILD_DIR}" ABSOLUTE)
get_filename_component(_disc_src_dir "${CMAKE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_disc_bin_dir "${CMAKE_BINARY_DIR}" ABSOLUTE)
if(NOT _disc_bin_dir STREQUAL DISC_BUILD_DIR)
  if(_disc_bin_dir STREQUAL _disc_src_dir)
    file(REMOVE "${CMAKE_SOURCE_DIR}/CMakeCache.txt")
    file(REMOVE_RECURSE "${CMAKE_SOURCE_DIR}/CMakeFiles")
  endif()
  file(MAKE_DIRECTORY "${DISC_BUILD_DIR}")
  set(_redir_args -S "${CMAKE_SOURCE_DIR}" -B "${DISC_BUILD_DIR}")
  if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _redir_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
  endif()
  if(CMAKE_BUILD_TYPE)
    list(APPEND _redir_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_redir_args}
    RESULT_VARIABLE _redir_rc
  )
  if(NOT _redir_rc EQUAL 0)
    message(FATAL_ERROR "Configuration failed for ${DISC_BUILD_DIR}")
  endif()
  message(FATAL_ERROR "Configured ${DISC_BUILD_REL}. Build it with: cmake --build ${DISC_BUILD_REL}")
endif()

set(DISC_BIN_DIR "${CMAKE_SOURCE_DIR}/build/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DISC_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DISC_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DISC_BIN_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")

set(SQLITE_ROOT ${CMAKE_SOURCE_DIR}/external/sqlite)
set(SQLITE_SOURCE ${SQLITE_ROOT}/sqlite3.c)
set(REDUMP_DB "${CMAKE_BINARY_DIR}/redump.db")
set(REDUMP_ASM "${CMAKE_BINARY_DIR}/redump_db.S")
set(REDUMP_OBJ "${CMAKE_BINARY_DIR}/redump_db.o")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "Release"))
  message(FATAL_ERROR "Please specify a build type of either \"Release\" or \"Debug\"\nCurrent Value: ${CMAKE_BUILD_TYPE}")
endif()
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_program(DISC_PYTHON NAMES python3 python REQUIRED)

add_custom_command(
  OUTPUT "${REDUMP_OBJ}"
  COMMAND ${DISC_PYTHON} "${CMAKE_SOURCE_DIR}/tools/build_redump_db.py"
          --out "${REDUMP_DB}"
          --asm "${REDUMP_ASM}"
  COMMAND ${CMAKE_C_COMPILER} -c -fPIC "${REDUMP_ASM}" -o "${REDUMP_OBJ}"
  DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/build_redump_db.py"
  BYPRODUCTS "${REDUMP_DB}" "${REDUMP_ASM}"
  VERBATIM
)

add_library(sqlite3 STATIC "${SQLITE_SOURCE}")
target_include_directories(sqlite3 PUBLIC "${SQLITE_ROOT}")
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_OS_OTHER=1
  SQLITE_THREADSAFE=0
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_WAL
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_OMIT_PROGRESS_CALLBACK
  SQLITE_OMIT_UTF16
  SQLITE_OMIT_DATETIME_FUNCS
  SQLITE_OMIT_DECLTYPE
  SQLITE_TEMP_STORE=3
  SQLITE_MAX_MMAP_SIZE=0
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_DQS=0
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
)
target_compile_options(sqlite3 PRIVATE -w -Os)

add_library(inih STATIC "${CMAKE_SOURCE_DIR}/external/inih/ini.c")
target_include_directories(inih PUBLIC "${CMAKE_SOURCE_DIR}/external/inih")
target_compile_definitions(inih PRIVATE
  INI_ALLOW_MULTILINE=0
  INI_ALLOW_NO_VALUE=1
  INI_MAX_LINE=512
)

add_library(miniz STATIC
  external/miniz/miniz.c
  external/miniz/miniz_tdef.c
)
target_include_directories(miniz PUBLIC "${CMAKE_SOURCE_DIR}/external/miniz")
target_compile_definitions(miniz PRIVATE
  MINIZ_NO_INFLATE_APIS
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
  MINIZ_NO_ZLIB_APIS
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES
)
target_compile_options(miniz PRIVATE -w -Os)

set_source_files_properties("${REDUMP_OBJ}" PROPERTIES
  EXTERNAL_OBJECT TRUE
  GENERATED TRUE
)

add_executable(disc
  src/main.c
  src/dump_loaded.c
  src/disc_type.c
  src/detect_disc.c
  src/disc_serial.c
  src/disc_mount.c
  src/disc_dump.c
  src/disc_hash.c
  src/disc_ird.c
  src/sfo.c
  src/param_json.cpp
  src/system_cnf.c
  src/disc_title.c
  src/redump.c
  src/sqlite_vfs.c
  "${REDUMP_OBJ}"
)
target_include_directories(disc PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/external/json"
)
target_link_libraries(disc PRIVATE sqlite3 inih miniz)
set_target_properties(disc PROPERTIES OUTPUT_NAME "${PAYLOAD_NAME}")
