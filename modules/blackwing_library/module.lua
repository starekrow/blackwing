-- The library: roots, the scanner and its priority queue, the sqlite index, change
-- watching, queries. Aim it at a directory and it is done; what is looked at is scanned
-- next. sqlite (public domain) is vendored and pinned, and built as C.
module "blackwing_library" {
  deps          = { "core", "platform", "blackwing_tags", "hifi_codec" },
  engine_module = true,
  test_deps     = { "cli", "blackwing_player", "hifi_platform" }, -- the heavy test plays what it finds
}
