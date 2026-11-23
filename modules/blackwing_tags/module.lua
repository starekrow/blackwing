-- Tags: read from every source (embedded tags, cue sheets, names and paths, folder
-- images), ranked by specificity, overridden by sidecars. Reads only; no library, no
-- database.
module "blackwing_tags" {
  deps = { "core" },
}
