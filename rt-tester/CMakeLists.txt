# CMakeList.txt : CMake project for rt-tester, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)
add_compile_definitions(DLL_EXPORTS)

add_executable (rt-tester "rt-tester.cpp" "rt-tester.h")

if (WIN32)
  find_package(PThreads4W REQUIRED)
  target_link_libraries(rt-tester PRIVATE PThreads4W::PThreads4W)
endif()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET rt-tester PROPERTY CXX_STANDARD 20)
endif()