cmake_minimum_required(VERSION 3.20)
project(panplay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PS5 工具链（ps5-payload-sdk 的 prospero.cmake）会设置 PS5。
option(PANPLAY_EMBED_UI "把 ui/ 打包进程序" ${PS5})
set(PANPLAY_CA_BUNDLE "" CACHE FILEPATH "PEM 根证书包；为空时下载 curl.se 的 cacert.pem（非 Windows）")

set(UNRAR_SOURCES
  third_party/unrar/rar.cpp
  third_party/unrar/strlist.cpp
  third_party/unrar/strfn.cpp
  third_party/unrar/pathfn.cpp
  third_party/unrar/smallfn.cpp
  third_party/unrar/global.cpp
  third_party/unrar/file.cpp
  third_party/unrar/filefn.cpp
  third_party/unrar/filcreat.cpp
  third_party/unrar/archive.cpp
  third_party/unrar/arcread.cpp
  third_party/unrar/unicode.cpp
  third_party/unrar/system.cpp
  third_party/unrar/crypt.cpp
  third_party/unrar/crc.cpp
  third_party/unrar/rawread.cpp
  third_party/unrar/encname.cpp
  third_party/unrar/resource.cpp
  third_party/unrar/match.cpp
  third_party/unrar/timefn.cpp
  third_party/unrar/rdwrfn.cpp
  third_party/unrar/consio.cpp
  third_party/unrar/options.cpp
  third_party/unrar/errhnd.cpp
  third_party/unrar/rarvm.cpp
  third_party/unrar/secpassword.cpp
  third_party/unrar/rijndael.cpp
  third_party/unrar/getbits.cpp
  third_party/unrar/sha1.cpp
  third_party/unrar/sha256.cpp
  third_party/unrar/blake2s.cpp
  third_party/unrar/hash.cpp
  third_party/unrar/extinfo.cpp
  third_party/unrar/extract.cpp
  third_party/unrar/volume.cpp
  third_party/unrar/list.cpp
  third_party/unrar/find.cpp
  third_party/unrar/unpack.cpp
  third_party/unrar/headers.cpp
  third_party/unrar/threadpool.cpp
  third_party/unrar/rs16.cpp
  third_party/unrar/cmddata.cpp
  third_party/unrar/ui.cpp
  third_party/unrar/largepage.cpp
  third_party/unrar/filestr.cpp
  third_party/unrar/scantree.cpp
  third_party/unrar/dll.cpp
  third_party/unrar/qopen.cpp
)
if(WIN32)
  list(APPEND UNRAR_SOURCES third_party/unrar/isnt.cpp third_party/unrar/motw.cpp)
endif()

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS core/src/*.cpp)
file(GLOB PC_SOURCES CONFIGURE_DEPENDS pc/src/*.cpp)

add_executable(panplay ${CORE_SOURCES} ${PC_SOURCES} ${UNRAR_SOURCES})
target_include_directories(panplay PRIVATE core/include pc/src third_party/unrar)
target_compile_definitions(panplay PRIVATE RARDLL _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
set_source_files_properties(${UNRAR_SOURCES} PROPERTIES CXX_STANDARD 11)

if(PS5)
  set_target_properties(panplay PROPERTIES OUTPUT_NAME panplay.elf)
  target_compile_definitions(panplay PRIVATE PANPLAY_PS5)
  target_link_libraries(panplay PRIVATE SceNotification)
endif()

if(WIN32)
  target_link_libraries(panplay PRIVATE winhttp ws2_32 z ole32 uuid advapi32 powrprof)
else()
  include(FetchContent)
  find_package(Threads REQUIRED)

  set_source_files_properties(${UNRAR_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-switch;$<$<CXX_COMPILER_ID:Clang>:-Wno-logical-op-parentheses>;$<$<CXX_COMPILER_ID:Clang>:-Wno-dangling-else>")
  target_compile_definitions(panplay PRIVATE RAR_SMP)

  # zlib 只取源码直接编进来，不走它自己的 CMake（它固定要编动态库）。
  FetchContent_Declare(zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    SOURCE_SUBDIR do-not-build)
  FetchContent_MakeAvailable(zlib)
  set(ZLIB_SOURCES adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees
                   uncompr zutil)
  list(TRANSFORM ZLIB_SOURCES PREPEND "${zlib_SOURCE_DIR}/")
  list(TRANSFORM ZLIB_SOURCES APPEND ".c")
  add_library(panplay_zlib STATIC ${ZLIB_SOURCES})
  target_include_directories(panplay_zlib PUBLIC "${zlib_SOURCE_DIR}")
  target_compile_definitions(panplay_zlib PRIVATE HAVE_UNISTD_H)

  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(GEN_FILES OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_user_config.h" CACHE FILEPATH "" FORCE)
  FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
    URL_HASH SHA256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110)
  FetchContent_MakeAvailable(mbedtls)

  if(PANPLAY_CA_BUNDLE STREQUAL "")
    set(PANPLAY_CA_BUNDLE "${CMAKE_BINARY_DIR}/cacert.pem")
    if(NOT EXISTS "${PANPLAY_CA_BUNDLE}")
      file(DOWNLOAD https://curl.se/ca/cacert.pem "${PANPLAY_CA_BUNDLE}" TLS_VERIFY ON STATUS ca_status)
      list(GET ca_status 0 ca_code)
      if(NOT ca_code EQUAL 0)
        file(REMOVE "${PANPLAY_CA_BUNDLE}")
        message(FATAL_ERROR "下载根证书失败：${ca_status}")
      endif()
    endif()
  endif()
  set(CA_INC "${CMAKE_BINARY_DIR}/generated/panplay_ca_bundle.inc")
  add_custom_command(OUTPUT "${CA_INC}"
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CA_INC} -DMODE=ca -DFILES=${PANPLAY_CA_BUNDLE}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake"
    DEPENDS "${PANPLAY_CA_BUNDLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake")
  target_sources(panplay PRIVATE "${CA_INC}")
  set_source_files_properties("${CA_INC}" PROPERTIES HEADER_FILE_ONLY ON)
  target_include_directories(panplay PRIVATE "${CMAKE_BINARY_DIR}/generated")
  target_compile_definitions(panplay PRIVATE PANPLAY_EMBEDDED_CA
    MBEDTLS_USER_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_user_config.h")
  target_link_libraries(panplay PRIVATE panplay_zlib mbedtls mbedx509 mbedcrypto Threads::Threads)
endif()

if(PANPLAY_EMBED_UI)
  set(UI_NAMES index.html app.css app.js)
  set(UI_FILES ${UI_NAMES})
  list(TRANSFORM UI_FILES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/ui/")
  set(UI_CPP "${CMAKE_BINARY_DIR}/generated/embedded_ui.cpp")
  # 自定义命令里的分号会被拆开，列表改用 | 分隔传给脚本。
  string(REPLACE ";" "|" UI_FILES_ARG "${UI_FILES}")
  string(REPLACE ";" "|" UI_NAMES_ARG "${UI_NAMES}")
  add_custom_command(OUTPUT "${UI_CPP}"
    COMMAND ${CMAKE_COMMAND} "-DOUTPUT=${UI_CPP}" -DMODE=ui "-DFILES=${UI_FILES_ARG}" "-DNAMES=${UI_NAMES_ARG}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake"
    DEPENDS ${UI_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake"
    VERBATIM)
  target_sources(panplay PRIVATE "${UI_CPP}")
  target_compile_definitions(panplay PRIVATE PANPLAY_EMBED_UI)
endif()

enable_testing()
add_test(NAME self-test COMMAND panplay self-test)
