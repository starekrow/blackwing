-- The application: the executable around the player, the library and a face. With a face
-- it is a window under Korvid's frame loop, paced so that the player's ticks never wait on
-- a frame; with none it plays the files named on its command line, or keeps a library and
-- plays from it, on a loop of its own that needs no display.
module "blackwing" {
  output        = "exe",
  deps          = { "core", "platform", "cli", "blackwing_player", "blackwing_library", "blackwing_glue", "hifi_platform",
                    "frameloop", "blackwing_window", "ui_dev" },
  engine_module = true,
}
