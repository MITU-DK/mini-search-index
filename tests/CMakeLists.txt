include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB MSE_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(mse_tests ${MSE_TEST_SOURCES})
target_link_libraries(mse_tests PRIVATE mse_app mse_core mse_server httplib::httplib
                      GTest::gtest_main)
target_compile_options(mse_tests PRIVATE ${MSE_WARNINGS})
target_compile_definitions(mse_tests PRIVATE
  MSE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  MSE_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../web")

include(GoogleTest)
gtest_discover_tests(mse_tests)
