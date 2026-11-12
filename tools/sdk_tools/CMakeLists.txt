# Small command-line tools built against the installed ReXGlue SDK.
#   cmake -S tools/sdk_tools -B out/sdk_tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_PREFIX_PATH=$REXSDK
#   cmake --build out/sdk_tools
cmake_minimum_required(VERSION 3.25)
project(fable2_sdk_tools CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(rexglue REQUIRED CONFIG)

foreach(tool extract dump flags)
  add_executable(${tool} ${tool}.cpp)
  target_link_libraries(${tool} PRIVATE rex::runtime)
  # Find librexruntime next to the SDK's other libraries.
  set_target_properties(${tool} PROPERTIES BUILD_RPATH "${REXGLUE_SHARE_DIR}/../../lib")
endforeach()
