-- The player: the queue, the transport, what plays next, and an API that is nothing but
-- events and tables on Korvid's busses. It drives hifi and is driven by anyone. No UI and
-- no library, so another project can link it and have a player.
module "blackwing_player" {
  deps          = { "core", "platform", "hifi", "hifi_codec", "hifi_eq", "hifi_platform" },
  engine_module = true,
  test_deps     = { "cli" },
}
