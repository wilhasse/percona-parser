# CMakeLists.txt for building a single "ib_parser" utility

cmake_minimum_required(VERSION 3.10)
project(MyIbParser LANGUAGES CXX)

# Set paths to percona-server
set(MYSQL_SOURCE_DIR "/home/cslog/percona-server" CACHE PATH "Path to percona-server source")
set(MYSQL_BUILD_DIR "/home/cslog/percona-server/build" CACHE PATH "Path to percona-server build")

# Example: C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wno-cast-qual -Wall -Wextra -Wno-unused-parameter)

# -- Provide a list of source files for decryption logic
set(DECRYPT_LIB_SOURCES
  decrypt.cc
  ibd_enc_reader.cc
  my_keyring_lookup.cc
  mysql_crc32c.cc
  keyring_stubs.cc
  
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

# -- Provide a list of source files for decompress logic
set(DECOMPRESS_LIB_SOURCES
  decompress.cc
)

# -- The main entry point: "ib_parser.cc"
set(IB_PARSER_MAIN
  ib_parser.cc
  parser.cc
  tables_dict.cc
  undrop_for_innodb.cc
)

# Create the executable
add_executable(ib_parser
  ${IB_PARSER_MAIN}
  ${DECRYPT_LIB_SOURCES}
  ${DECOMPRESS_LIB_SOURCES}
)

# Add RapidJSON include path
set(RAPIDJSON_INCLUDE_DIR "/home/cslog/rapidjson/include")

# Verify that the path exists
if(NOT EXISTS "${RAPIDJSON_INCLUDE_DIR}")
    message(FATAL_ERROR "RapidJSON include directory not found: ${RAPIDJSON_INCLUDE_DIR}")
endif()

# Include directories
target_include_directories(ib_parser
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MYSQL_SOURCE_DIR}
    ${MYSQL_SOURCE_DIR}/storage/innobase
    ${MYSQL_SOURCE_DIR}/storage/innobase/include
    ${MYSQL_SOURCE_DIR}/include
    ${MYSQL_SOURCE_DIR}/sql
    ${MYSQL_SOURCE_DIR}/plugin/keyring
    ${MYSQL_SOURCE_DIR}/plugin/keyring/common
    ${MYSQL_BUILD_DIR}/include
    ${RAPIDJSON_INCLUDE_DIR}
)

# Link directories
link_directories(
    ${MYSQL_BUILD_DIR}/archive_output_directory
    ${MYSQL_BUILD_DIR}/storage/innobase
    ${MYSQL_BUILD_DIR}/mysys
    ${MYSQL_BUILD_DIR}/strings
    ${MYSQL_BUILD_DIR}/extra/lz4/lz4_lib
)

# Compile definitions
target_compile_definitions(ib_parser
  PRIVATE
    UNIV_NO_ERR_MSGS
    UNIV_LIBRARY
    DISABLE_PSI_MEMORY
    MYSQL_SERVER
    ER_TOO_MANY_ERROR_CODES=9999
    ER_TOO_BIG_ERROR_CODE=9998
    DBUG_OFF
    NDEBUG
    DISABLE_PSI_FILE
    DISABLE_PSI_RWLOCK
    DISABLE_PSI_MUTEX
    DISABLE_PSI_THREAD
)

# Link libraries
target_link_libraries(ib_parser
  PRIVATE
    ${MYSQL_BUILD_DIR}/archive_output_directory/libmysys.a
    ${MYSQL_BUILD_DIR}/storage/innobase/libinnodb_zipdecompress.a
    ${MYSQL_BUILD_DIR}/archive_output_directory/liblz4_lib.a
    ${MYSQL_BUILD_DIR}/archive_output_directory/libstrings.a
    ${MYSQL_BUILD_DIR}/archive_output_directory/libperconaserverclient.a
    z
    ssl
    crypto
    pthread
    dl
)

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
  PROPERTIES
    COMPILE_FLAGS
      "-DLogPluginErr\\(level,errcode,message\\)=do{}while\\(0\\) -DLogPluginErrV\\(level,errcode,vl\\)=do{}while\\(0\\) -Wno-unused-parameter"
)

# Done. "make" => builds "ib_parser".