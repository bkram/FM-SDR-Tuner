#include <iostream>

#include "app_options.h"
#include "application.h"
#include "calibration.h"

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

  if (parse.options.calibrate) {
    fm_tuner::calibration::BandSweepOptions sweep;
    sweep.deviceIndex = parse.options.rtlDeviceIndex;
    sweep.sampleRateHz = parse.options.iqSampleRate;
    sweep.gainTenthsDb = parse.options.gain;
    return fm_tuner::calibration::runBandSweep(sweep);
  }

  Application app(parse.options);
  return app.run();
}
