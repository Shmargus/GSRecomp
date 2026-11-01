// The Broken Seal's supported ROM revisions: the one list. Each has its own
// game code (local\<code>\gs011, USA/Europe local\gs011) and its own
// config\<code> (USA/Europe: config\usa). Row order is the language number the
// launcher's flags store (0 USA/Europe .. 4 Germany); gs::RomVersion follows it.
//   GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator)
// stamp_*: the two ROM routines player_builder runs to make the stamps,
// measured per revision (FACTS.md "Spanish overlays and transient RAM code").
// Read by src/rom_version.cpp, launcher/launcher_game.h,
// player_builder/builder_main.cpp and scripts/build_game.ps1, which parses
// these lines: keep one row per line.
GSR_ROM_REVISION("en", "USA/Europe", "5c4695205413df7db52b9a184815a07783999971", 0x080ED408u, 0x080048B0u)
GSR_ROM_REVISION("es", "Spain", "de0bdc889594b012cad213ca83707feb81ada28b", 0x080F0C08u, 0x08004910u)
GSR_ROM_REVISION("it", "Italy", "ca45e04b26b2f040ac63e679a459d21869f5f28d", 0x080ED408u, 0x080048F0u)
GSR_ROM_REVISION("fr", "France", "42f3b262c16cfc5bde1fa2b25016fb74046de1b3", 0x080F0C08u, 0x080048C0u)
GSR_ROM_REVISION("de", "Germany", "c23ab91a39434623cf1a999f387d743ade1a22a6", 0x080EF208u, 0x080048E0u)
