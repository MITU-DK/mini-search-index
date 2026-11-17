#include <unistd.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

#include "cli.hpp"

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  // Bold highlights only when printing to a terminal, and never if NO_COLOR is set.
  const bool color = isatty(STDOUT_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr;
  try {
    return mse::run_cli(args, std::cout, std::cerr, color);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
