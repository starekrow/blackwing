-- The decoder contract and the decoders: a readable handle in; a format, gapless facts
-- and PCM out. A decoder opens nothing and calls back nothing, so it can move into a
-- sandboxed process later unchanged.
module "hifi_codec" {
  deps = { "core" },
}
