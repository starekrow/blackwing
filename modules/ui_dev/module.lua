-- The development face: every table and event visible, the pipeline inspectable. Built
-- first; how Blackwing is developed. A customer of the player's and the library's API
-- and nothing more: it depends on them for their declarations and calls no function of
-- either (its tests run with neither in the engine).
module "ui_dev" {
  deps          = { "core", "platform", "platform_hid", "asset", "krome", "kim", "blackwing_player", "blackwing_library", "blackwing_session" },
  engine_module = true,
  test_deps     = { "cli", "blackwing_glue" }, -- the test over the player and the library themselves needs what joins them
}
