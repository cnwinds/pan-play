# 把文件变成 C++ 字节数组。用法：
#   cmake -DOUTPUT=<out.cpp> -DMODE=ui -DFILES="a|b" -DNAMES="a|b" -P embed_files.cmake
#   cmake -DOUTPUT=<out.inc> -DMODE=ca -DFILES=<cacert.pem> -P embed_files.cmake
#   cmake -DOUTPUT=<out.inc> -DMODE=gz -DFILES=<catalog.json> -P embed_files.cmake
#   cmake -DOUTPUT=<out.inc> -DMODE=icon -DFILES=<icon0.png> -P embed_files.cmake
# ui 模式生成 panplay_embedded::files 表；ca 模式生成以 0 结尾的 kCaBundle，供 mbedTLS 按 PEM 解析；
# gz 模式先 gzip 压缩，生成 kCatalogGz；icon 模式原样生成 kIcon0Png。

function(bytes_of path out_var)
  file(READ "${path}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(REGEX REPLACE "((0x..,){32})" "\\1\n" hex "${hex}")
  set(${out_var} "${hex}" PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "ca")
  bytes_of("${FILES}" bytes)
  file(WRITE "${OUTPUT}.tmp" "static const unsigned char kCaBundle[] = {\n${bytes}0x00};\n")
elseif(MODE STREQUAL "gz")
  file(ARCHIVE_CREATE OUTPUT "${OUTPUT}.gz" PATHS "${FILES}" FORMAT raw COMPRESSION GZip)
  bytes_of("${OUTPUT}.gz" bytes)
  file(REMOVE "${OUTPUT}.gz")
  file(WRITE "${OUTPUT}.tmp" "static const unsigned char kCatalogGz[] = {\n${bytes}};\n")
elseif(MODE STREQUAL "icon")
  bytes_of("${FILES}" bytes)
  file(WRITE "${OUTPUT}.tmp" "static const unsigned char kIcon0Png[] = {\n${bytes}};\n")
else()
  string(REPLACE "|" ";" FILES "${FILES}")
  string(REPLACE "|" ";" NAMES "${NAMES}")
  set(body "#include \"embedded_ui.hpp\"\n\nnamespace panplay_embedded {\nnamespace {\n")
  set(table "")
  list(LENGTH FILES count)
  math(EXPR last "${count} - 1")
  foreach(index RANGE ${last})
    list(GET FILES ${index} path)
    list(GET NAMES ${index} name)
    bytes_of("${path}" bytes)
    string(APPEND body "const unsigned char file_${index}[] = {\n${bytes}0x00};\n")
    string(APPEND table "    {\"${name}\", file_${index}, sizeof(file_${index}) - 1},\n")
  endforeach()
  string(APPEND body "}  // namespace\n\nconst File files[] = {\n${table}};\nconst std::size_t file_count = ${count};\n\n}  // namespace panplay_embedded\n")
  file(WRITE "${OUTPUT}.tmp" "${body}")
endif()

# 内容没变就不动文件，免得每次都重新编译。
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}")
file(REMOVE "${OUTPUT}.tmp")
