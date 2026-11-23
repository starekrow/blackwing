-- The hard-realtime audio path and its interfaces: sources through stages into the lead,
-- the join, the output callback, the clock at the DAC. It knows nothing of tracks, queues,
-- tags or players: it renders what it is given and says what it did.
module "hifi" {
  deps = { "core", "platform", "hifi_codec", "hifi_platform" },
}
