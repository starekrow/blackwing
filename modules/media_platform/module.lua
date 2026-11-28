-- The OS media surface, per OS: media keys in, now-playing out, keep-awake,
-- interruptions. A small subscribe-and-send layer; the player does not know it
-- exists. Linux's is MPRIS, keep-awake and sleep over dbus.
module "media_platform" {
  deps      = { "core", "platform", "dbus" },
  test_deps = { "dbus" },
}
