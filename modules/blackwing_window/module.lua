-- What every face shares: the window's side of the application. For now, that closing the
-- window ends the run. It is in the engine only when a window is (blackwing's headless
-- commands leave it out), so it may name the frame loop's and the window's events, which a
-- machine with no display never registers.
module "blackwing_window" {
  deps          = { "core", "platform", "platform_hid", "frameloop" },
  engine_module = true,
}
