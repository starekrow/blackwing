-- The development face: every table and event visible, the pipeline inspectable. Built
-- first; how Blackwing is developed.
module "ui_dev" {
  deps = { "core", "krome", "hifi", "blackwing_player", "blackwing_library" },
}
