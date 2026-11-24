-- Where the player and the library hear each other. Neither knows the other: this module
-- knows both, and is the only thing that does besides the faces. A file queued is the
-- library's priority; what the library is asked to collect is queued.
module "blackwing_glue" {
  deps          = { "core", "blackwing_player", "blackwing_library" },
  engine_module = true,
}
