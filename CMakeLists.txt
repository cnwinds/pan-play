cmake_minimum_required(VERSION 3.20)
project(panplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
  third_party/unrar/isnt.cpp
  third_party/unrar/motw.cpp
)

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS core/src/*.cpp)
file(GLOB PC_SOURCES CONFIGURE_DEPENDS pc/src/*.cpp)

add_executable(panplay ${CORE_SOURCES} ${PC_SOURCES} ${UNRAR_SOURCES})
target_include_directories(panplay PRIVATE core/include pc/src third_party/unrar)
target_compile_definitions(panplay PRIVATE RARDLL _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
set_source_files_properties(${UNRAR_SOURCES} PROPERTIES CXX_STANDARD 11)

if(WIN32)
  target_link_libraries(panplay PRIVATE winhttp ws2_32 z ole32 uuid advapi32 powrprof)
endif()
