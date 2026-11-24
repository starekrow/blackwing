-- The script host: Lua over the player's and the library's API, by the names native code
-- uses, made from the field tables the two modules carry. This slice: one script the user
-- runs (`blackwing run FILE.lua`), in process and trusted. The rest of the script-module
-- spec (modules found on disk, a manifest, capabilities, a sandbox) is P1, and is built
-- on this.
module "blackwing_script" {
  deps          = { "core", "platform", "lua", "script", "blackwing_player", "blackwing_library" },
  engine_module = true,
  test_deps     = { "cli", "blackwing_glue", "hifi_platform" },
}
