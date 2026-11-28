#include "search_cli.h"

int main(int argc, char** argv) {
  search_engine::CliOptions options;
  int code = search_engine::ParseArgs(argc, argv, &options);
  if (code != 0) {
    return code;
  }
  return search_engine::Run(options);
}
