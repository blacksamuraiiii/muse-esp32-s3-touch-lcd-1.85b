# Third-party sources that are compiled into the muse component

These live here, and not as their own component directory, because of how this
project builds offline.

`pcf85063a` is Waveshare's driver for the 1.85B's RTC, taken unmodified:

    waveshare/pcf85063a v2.1.0
    https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/sensor/pcf85063a
    commit 30d0ac3b8b6b27ebd402c5852819cbe0dab92749, path sensor/pcf85063a

It is normally an `idf_component.yml` dependency. Two things rule that out on
this build host:

1. The registry solve fetches every dependency's manifest, and this host cannot
   reliably reach `components-file.espressif.com` (CloudFront resolves to a
   dozen edge addresses, most of which refuse the connection).
2. Adding *any* new component - even one with no manifest, vendored and local -
   changes the set of direct dependencies, which makes the manager re-solve.
   So "just vendor it as its own component" is not an offline escape either.

Compiling it into `muse` changes neither the manifest hash nor the component
set, so an existing `dependencies.lock` stays valid and the build runs offline.

To go back to the registry version: delete this directory, remove the entry
from `../CMakeLists.txt`, restore `#include "pcf85063a.h"` in `../muse_rtc.c`,
and add the dependency back to `../idf_component.yml`:

    waveshare/pcf85063a:
      version: "^2.1.0"
      rules:
        - if: "$CONFIG{MUSE_BOARD_ID} == \"waveshare_s3_touch_lcd_185b\""
