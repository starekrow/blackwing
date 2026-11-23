-- The decoder contract and the decoders: the file's bytes in; a format, gapless facts and
-- PCM as integers out. A decoder opens nothing, allocates only from memory it is handed and
-- calls back nothing, so it can move into a sandboxed process later unchanged. FLAC is
-- dr_flac (public domain or MIT-0), vendored and pinned.
module "hifi_codec" {
  deps     = { "core" },
  licenses = { "src/vendor/LICENSE" },
}
