-- The decoder contract and the decoders: a file as a sparse span in; a format, gapless
-- facts and PCM as integers out, from any frame. A decoder opens nothing, allocates only
-- from memory it is handed, calls back nothing and waits by returning, so it can move
-- into a sandboxed process later unchanged. FLAC is dr_flac and MP3 is dr_mp3 (each public
-- domain or MIT-0), vendored and pinned.
module "hifi_codec" {
  deps     = { "core" },
  licenses = { "src/vendor/LICENSE", "src/vendor/LICENSE.dr_mp3" },
  codegen_sources = { wrapv = { "src/flac.cc" } }, -- a damaged file's predictor sums wrap
}
