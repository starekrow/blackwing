-- The application: the executable around the player and the library. For now it is
-- headless: it plays the files named on its command line, or keeps a library and plays
-- from it, says what is happening, and exits when told to (or when the queue is done). Its own small loop, not tool_main's: a player sits for hours and must sleep.
module "blackwing" {
  output        = "exe",
  deps          = { "core", "platform", "cli", "blackwing_player", "blackwing_library", "hifi_platform" },
  engine_module = true,
}
