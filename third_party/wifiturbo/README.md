# Wi-Fi Turbo interface

Declarations and import stubs for the optional
[vita-turbo-wifi](https://github.com/juanjsebgarcia/vita-turbo-wifi) plugin.

- `wifiturbo_api.h`: the declarations Moonlight uses, copied from the plugin
- `wifiturbo_imports.yml`: the same functions as an import list. CMake turns it into
  `libSceWifiTurbo_stub_weak.a` with `vita-libs-gen`, so Moonlight starts without the plugin.
- `LICENSE`: the plugin's licence (MIT). Moonlight is GPLv3, which MIT code can be
  combined with as long as this notice stays with the copied declarations.

The application code that uses them is `src/wifiturbo.c`.
