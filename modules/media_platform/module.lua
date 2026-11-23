-- The OS media surface, per OS: media keys in, now-playing out, keep-awake,
-- interruptions. A small subscribe-and-send layer; the player does not know it
-- exists.
module "media_platform" {
  deps = { "core", "platform" },
}
