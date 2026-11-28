-- The OS's media surface joined to the busses: what the player is playing, from the
-- library's titles and the art beside the files, goes out through media_platform; what
-- the OS asks (the keys, the shell's controls, MPRIS) comes in as the player's commands.
-- Keep-awake while playing, and sleep as an interruption. The player does not know it
-- exists; it is application glue, as blackwing_glue is.
module "blackwing_media" {
  deps          = { "core", "platform", "blackwing_player", "blackwing_library", "blackwing_tags", "hifi_codec", "media_platform" },
  test_deps     = { "cli", "dbus", "hifi_platform" },
  engine_module = true,
}
