-- Tags: what a file's bytes say about it, from every source (ID3v2, Vorbis comments,
-- APEv2, ID3v1, the path), ranked. Functions of bytes over a sparse span: they open
-- nothing and allocate nothing, and the library feeds them. Reads only; no library, no
-- database.
module "blackwing_tags" {
  deps       = { "core", "hifi_codec" }, -- hifi_codec for the sparse span, the decoders' kind of file
}
