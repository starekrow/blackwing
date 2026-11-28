-- What every face shares: the window's side of the application. Closing the window while
-- it plays hides it, and the media surface's Raise brings it back; closed otherwise, the
-- run ends. It is in the engine only when a window is (blackwing's headless
-- commands leave it out), so it may name the frame loop's and the window's events, which a
-- machine with no display never registers.
module "blackwing_window" {
  deps          = { "core", "platform", "platform_hid", "frameloop", "blackwing_player", "blackwing_media" },
  engine_module = true,
}
