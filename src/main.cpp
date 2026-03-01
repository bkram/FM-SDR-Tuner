#include <iostream>

#include "app_options.h"
#include "application.h"

int main(int argc, char *argv[]) {
  constexpr int kInputRate = 256000;

  std::cout << "FM-SDR-Tuner version " << FM_SDR_TUNER_VERSION << "\n"
            << "Copyright 2026 by Bkram Developments\n";

  const AppParseResult parse = parseAppOptions(argc, argv, kInputRate);
  if (parse.outcome == AppParseOutcome::ExitSuccess) {
    return 0;
  }
  if (parse.outcome == AppParseOutcome::ExitFailure) {
    return 1;
  }

  Application app(parse.options);
  return app.run();
}
