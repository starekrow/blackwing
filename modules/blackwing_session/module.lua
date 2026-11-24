-- The session: how the application was left, there at the next start (B28). What was
-- queued, where it had got to, the chain's parameters, and whatever a face asks to have
-- kept. It is a customer of the player like any other: it reads the tables to save and
-- emits commands to restore. The player has no files of its own.
module "blackwing_session" {
  deps          = { "core", "platform", "blackwing_player" },
  engine_module = true,
  test_deps     = { "cli", "hifi_platform" },
}
