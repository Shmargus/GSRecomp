// The Broken Seal's supported ROM revisions: the one list. Each has its own
// game code (local\<code>\gs011, USA/Europe local\gs011) and its own
// config\<code> (USA/Europe: config\usa). The first five rows are the
// languages, in the order of the number the launcher's flags store (0
// USA/Europe .. 4 Germany); gs::RomVersion follows them. Later rows are
// patched ROMs with their own game code (Reloaded: FACTS.md "Golden Sun
// Reloaded"). `language` is the language row a revision plays as; the
// launcher picks a patched revision by the ROM's SHA-1, never by a flag.
//   GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language)
// stamp_*: the two ROM routines player_builder runs to make the stamps,
// measured per revision (FACTS.md "Spanish overlays and transient RAM code").
// Read by src/rom_version.cpp, launcher/launcher_game.h,
// player_builder/builder_main.cpp and scripts/build_game.ps1, which parses
// these lines: keep one row per line.
GSR_ROM_REVISION("en", "USA/Europe", "5c4695205413df7db52b9a184815a07783999971", 0x080ED408u, 0x080048B0u, "en")
GSR_ROM_REVISION("es", "Spain", "de0bdc889594b012cad213ca83707feb81ada28b", 0x080F0C08u, 0x08004910u, "es")
GSR_ROM_REVISION("it", "Italy", "ca45e04b26b2f040ac63e679a459d21869f5f28d", 0x080ED408u, 0x080048F0u, "it")
GSR_ROM_REVISION("fr", "France", "42f3b262c16cfc5bde1fa2b25016fb74046de1b3", 0x080F0C08u, 0x080048C0u, "fr")
GSR_ROM_REVISION("de", "Germany", "c23ab91a39434623cf1a999f387d743ade1a22a6", 0x080EF208u, 0x080048E0u, "de")
GSR_ROM_REVISION("rl", "Reloaded 1.83a", "448e73f566679fa87980d0eb46b32f395fbc231d", 0x080ED408u, 0x080048B0u, "en")
GSR_ROM_REVISION("pt", "Portuguese Beta 1.01", "17bc59e18ff59dc2ff20b8c04c247f4a789f8780", 0x080ED408u, 0x080048B0u, "en")
