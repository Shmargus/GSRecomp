// The Lost Age's supported ROM revisions: the one list. Each has its own
// game code (local\<code>\tla, USA/Europe local\tla) and its own
// config\<code> (USA/Europe: config\tla). Row order is the language number
// the launcher's flags store (0 USA/Europe .. 4 Germany); tla_rom::Version
// follows it.
//   GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator)
// stamp_*: the two ROM routines player_builder runs to make the stamps
// (tla_generate.h), measured per revision.
// Read by src/tla_rom_version.cpp, launcher/launcher_game.h,
// player_builder/builder_main.cpp and scripts/build_game.ps1, which parses
// these lines: keep one row per line.
GSR_ROM_REVISION("en", "USA/Europe", "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824", 0x08196404u, 0x08014CC0u)
GSR_ROM_REVISION("es", "Spain", "a2e3e80036195343c2713410ac14f4e329e3f40d", 0x08196404u, 0x08014CECu)
GSR_ROM_REVISION("it", "Italy", "57f1d005666b81645e55412658427e27099c5907", 0x08196404u, 0x08014CECu)
GSR_ROM_REVISION("fr", "France", "f14855c0f8c87a95cc52189e8e3b2bd9df186322", 0x08196404u, 0x08014CECu)
GSR_ROM_REVISION("de", "Germany", "7820b4c7ba9002b0e9be23a6f45a9ab204986438", 0x08196404u, 0x08014CECu)
