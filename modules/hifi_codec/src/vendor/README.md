# Vendored

`dr_flac.h`: dr_flac 0.13.5 (in development) by David Reid, public domain or MIT-0, from
https://github.com/mackron/dr_libs at commit 09e1d68709127b32e74c0029e04198df6ee0b4b2
(sha256 bd569c910a7c465e481fd50af84e2871e81e2154e30ac510d98f953cc0d5a546), unmodified.
`LICENSE` is the licence statement from the end of that file.

Only the native FLAC decoder over a block of memory is compiled: `src/flac.cc` includes it
with stdio, Ogg and wide-character paths configured out, its asserts off, and its
fallback `malloc`, `realloc` and `free` replaced by ones that return nothing, so it can
only use the memory the decoder contract hands it.

Upgrading is replacing the file and the commit and hash above, then running the module's
tests (`kormake test hifi_codec`).
