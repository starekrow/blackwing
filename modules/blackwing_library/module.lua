-- The library: roots, the scanner and its priority queue, the sqlite index, change
-- watching, queries. Aim it at a directory and it is done; what is looked at is scanned
-- next.
module "blackwing_library" {
  deps = { "core", "platform", "blackwing_tags" },
}
