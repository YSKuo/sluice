#include <cstring>
#include <iostream>

#include "version.h"

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      std::cout << "sluice " << sluice::GetVersion() << "\n";
      return 0;
    }
  }

  std::cout << "sluice " << sluice::GetVersion() << " (gateway not yet implemented)\n";
  return 0;
}
