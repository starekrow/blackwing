-- The application: the executable around the player. For now it is headless: it plays the
-- files named on its command line, says what is happening, and exits when the queue is
-- done. Its own small loop, not tool_main's: a player sits for hours and must sleep.
module "blackwing" {
  output        = "exe",
  deps          = { "core", "platform", "cli", "blackwing_player", "hifi_platform" },
  engine_module = true,
}
