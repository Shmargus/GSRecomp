// The Lost Age's supported ROM revisions: the one list. Each has its own
// game code (local\<code>\tla, USA/Europe local\tla) and its own
// config\<code> (USA/Europe: config\tla). Row order is the language number
// the launcher's flags store (0 USA/Europe .. 4 Germany); tla_rom::Version
// follows it.
//   GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language, romhack)
// stamp_*: the two ROM routines player_builder runs to make the stamps
// (tla_generate.h), measured per revision.
// Read by src/tla_rom_version.cpp, launcher/launcher_game.h,
// player_builder/builder_main.cpp and scripts/build_game.ps1, which parses
// these lines: keep one row per line.
GSR_ROM_REVISION("en", "USA/Europe", "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824", 0x08196404u, 0x08014CC0u, "en", false)
GSR_ROM_REVISION("es", "Spain", "a2e3e80036195343c2713410ac14f4e329e3f40d", 0x08196404u, 0x08014CECu, "es", false)
GSR_ROM_REVISION("it", "Italy", "57f1d005666b81645e55412658427e27099c5907", 0x08196404u, 0x08014CECu, "it", false)
GSR_ROM_REVISION("fr", "France", "f14855c0f8c87a95cc52189e8e3b2bd9df186322", 0x08196404u, 0x08014CECu, "fr", false)
GSR_ROM_REVISION("de", "Germany", "7820b4c7ba9002b0e9be23a6f45a9ab204986438", 0x08196404u, 0x08014CECu, "de", false)
GSR_ROM_REVISION("rl", "Reloaded 1.83", "e045d22fabbed374dd0e1a73f7a1f8d2c5cdba40", 0x08196404u, 0x08014CC0u, "en", true)
GSR_ROM_REVISION("pt", "Portuguese Beta 1", "8deafd796bf44e90da39fdb832fb70db530b598d", 0x08196404u, 0x08014CC0u, "en", false)
GSR_ROM_REVISION("mh", "Maheo Hack 2.0", "6325d2e3463f19dd1b575b64024b1704431dfd1d", 0x08196404u, 0x08014CC0u, "en", true)
