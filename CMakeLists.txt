# CMakeLists.txt for building InnoDB parser utilities and library

cmake_minimum_required(VERSION 3.10)
project(IbdReader LANGUAGES CXX)

# Build options
option(BUILD_EXECUTABLE "Build the ib_parser executable" ON)
option(BUILD_SHARED_LIB "Build the shared library" ON)
option(BUILD_STATIC_LIB "Build the static library" OFF)

# Set paths to percona-server
set(MYSQL_SOURCE_DIR "/home/cslog/mysql/percona-server" CACHE PATH "Path to percona-server source")
set(MYSQL_BUILD_DIR "/home/cslog/mysql/percona-server/build" CACHE PATH "Path to percona-server build")

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Position-independent code for library builds
if(BUILD_SHARED_LIB OR BUILD_STATIC_LIB)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Compile options
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Core sources shared by both executable and library
set(CORE_SOURCES
    decompress.cc
    decrypt.cc
    parser.cc
    tables_dict.cc
    undrop_for_innodb.cc
    ibd_enc_reader.cc
    my_keyring_lookup.cc
    mysql_crc32c.cc
    keyring_stubs.cc
    ${MYSQL_SOURCE_DIR}/storage/innobase/ut/crc32.cc
    
    # Plugin/keyring sources from percona-server
    ${MYSQL_SOURCE_DIR}/plugin/keyring/buffered_file_io.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keyring_key.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keyring_impl.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keys_container.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/digest.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/converter.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/file_io.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/buffer.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker_factory.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker_ver_1_0.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker_ver_2_0.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keys_iterator.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/hash_to_buffer_serializer.cc
    ${MYSQL_SOURCE_DIR}/strings/ctype-latin1.cc
)

# Library-specific sources
set(LIBRARY_SOURCES
    lib/ibd_reader_api.cc
)

# Executable-specific sources
set(EXECUTABLE_SOURCES
    ib_parser.cc
)

# Include directories
set(COMMON_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MYSQL_SOURCE_DIR}
    ${MYSQL_SOURCE_DIR}/storage/innobase
    ${MYSQL_SOURCE_DIR}/storage/innobase/include
    ${MYSQL_SOURCE_DIR}/include
    ${MYSQL_SOURCE_DIR}/sql
    ${MYSQL_SOURCE_DIR}/plugin/keyring
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common
    ${MYSQL_BUILD_DIR}/include
    /home/cslog/rapidjson/include
)

# Link directories
link_directories(
    ${MYSQL_BUILD_DIR}/archive_output_directory
    ${MYSQL_BUILD_DIR}/storage/innobase
    ${MYSQL_BUILD_DIR}/mysys
    ${MYSQL_BUILD_DIR}/strings
    ${MYSQL_BUILD_DIR}/extra/lz4/lz4_lib
)

# Common compile definitions
set(COMMON_COMPILE_DEFS
    UNIV_NO_ERR_MSGS
    UNIV_LIBRARY
    DISABLE_PSI_MEMORY
    MYSQL_SERVER
    DBUG_OFF
    NDEBUG
    DISABLE_PSI_FILE
    DISABLE_PSI_RWLOCK
    DISABLE_PSI_MUTEX
    DISABLE_PSI_THREAD
)

# Static libraries to link
set(STATIC_LIBRARIES
    ${MYSQL_BUILD_DIR}/archive_output_directory/libmysys.a
    ${MYSQL_BUILD_DIR}/storage/innobase/libinnodb_zipdecompress.a
    ${MYSQL_BUILD_DIR}/archive_output_directory/liblz4_lib.a
    ${MYSQL_BUILD_DIR}/archive_output_directory/libstrings.a
    ${MYSQL_BUILD_DIR}/archive_output_directory/libperconaserverclient.a
)

# System libraries
set(SYSTEM_LIBRARIES
    z
    ssl
    crypto
    pthread
    dl
)

# ============================================================================
# Build Shared Library
# ============================================================================
if(BUILD_SHARED_LIB)
    add_library(ibd_reader_shared SHARED
        ${CORE_SOURCES}
        ${LIBRARY_SOURCES}
    )
    
    set_target_properties(ibd_reader_shared PROPERTIES
        OUTPUT_NAME ibd_reader
        VERSION 1.0.0
        SOVERSION 1
        PUBLIC_HEADER "lib/ibd_reader_api.h"
    )
    
    target_include_directories(ibd_reader_shared 
        PUBLIC 
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
            $<INSTALL_INTERFACE:include>
        PRIVATE 
            ${COMMON_INCLUDE_DIRS}
    )
    
    target_compile_definitions(ibd_reader_shared
        PRIVATE
            ${COMMON_COMPILE_DEFS}
            IBD_READER_EXPORTS
    )
    
    # Control symbol visibility
    set_target_properties(ibd_reader_shared PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    
    target_link_libraries(ibd_reader_shared
        PRIVATE
            ${STATIC_LIBRARIES}
            ${SYSTEM_LIBRARIES}
    )
    
    install(TARGETS ibd_reader_shared
        LIBRARY DESTINATION lib
        PUBLIC_HEADER DESTINATION include
    )
endif()

# ============================================================================
# Build Static Library
# ============================================================================
if(BUILD_STATIC_LIB)
    add_library(ibd_reader_static STATIC
        ${CORE_SOURCES}
        ${LIBRARY_SOURCES}
    )
    
    set_target_properties(ibd_reader_static PROPERTIES
        OUTPUT_NAME ibd_reader
        PUBLIC_HEADER "lib/ibd_reader_api.h"
    )
    
    target_include_directories(ibd_reader_static
        PUBLIC 
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
            $<INSTALL_INTERFACE:include>
        PRIVATE 
            ${COMMON_INCLUDE_DIRS}
    )
    
    target_compile_definitions(ibd_reader_static
        PRIVATE
            ${COMMON_COMPILE_DEFS}
    )
    
    install(TARGETS ibd_reader_static
        ARCHIVE DESTINATION lib
        PUBLIC_HEADER DESTINATION include
    )
endif()

# ============================================================================
# Build Executable
# ============================================================================
if(BUILD_EXECUTABLE)
    add_executable(ib_parser
        ${EXECUTABLE_SOURCES}
        ${CORE_SOURCES}
    )
    
    target_include_directories(ib_parser
        PRIVATE
            ${COMMON_INCLUDE_DIRS}
    )
    
    target_compile_definitions(ib_parser
        PRIVATE
            ${COMMON_COMPILE_DEFS}
    )
    
    target_link_libraries(ib_parser
        PRIVATE
            ${STATIC_LIBRARIES}
            ${SYSTEM_LIBRARIES}
    )
    
    install(TARGETS ib_parser
        RUNTIME DESTINATION bin
    )
endif()

# Set source file properties to disable certain warnings and define macros
set_source_files_properties(
    ${MYSQL_SOURCE_DIR}/plugin/keyring/buffered_file_io.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keyring_key.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keyring_impl.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keys_container.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/file_io.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/buffer.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker_factory.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker_ver_1_0.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/checker/checker_ver_2_0.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common/keys_iterator.cc
    ${MYSQL_SOURCE_DIR}/plugin/keyring/hash_to_buffer_serializer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/ib_parser.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/ibd_enc_reader.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/my_keyring_lookup.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/ibd_reader_api.cc
    PROPERTIES
        COMPILE_FLAGS
            "-DLogPluginErr\\(level,errcode,message\\)=do{}while\\(0\\) -DLogPluginErrV\\(level,errcode,vl\\)=do{}while\\(0\\) -Wno-unused-parameter"
)

# Print configuration summary
message(STATUS "========================================")
message(STATUS "InnoDB Reader Build Configuration:")
message(STATUS "  Build Executable: ${BUILD_EXECUTABLE}")
message(STATUS "  Build Shared Library: ${BUILD_SHARED_LIB}")
message(STATUS "  Build Static Library: ${BUILD_STATIC_LIB}")
message(STATUS "  MySQL Source: ${MYSQL_SOURCE_DIR}")
message(STATUS "  MySQL Build: ${MYSQL_BUILD_DIR}")
message(STATUS "========================================")
