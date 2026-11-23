# Vendored

`dr_flac.h`: dr_flac 0.13.5 (in development) by David Reid, public domain or MIT-0, from
https://github.com/mackron/dr_libs at commit 09e1d68709127b32e74c0029e04198df6ee0b4b2
(sha256 bd569c910a7c465e481fd50af84e2871e81e2154e30ac510d98f953cc0d5a546), unmodified.
`LICENSE` is the licence statement from the end of that file.

Only the native FLAC decoder over a block of memory is compiled: `src/flac.cc` includes it
with stdio, Ogg and wide-character paths configured out, its asserts off, and its
fallback `malloc`, `realloc` and `free` replaced by ones that return nothing, so it can
only use the memory the decoder contract hands it.

`dr_mp3.h`: dr_mp3 0.7.5 (in development) by David Reid, on minimp3 by lieff, public domain
or MIT-0 (minimp3 CC0), from the same repository at the same commit (sha256
7a6cc38b0afdb55cfc9eec991d9169d67beb8fd910be2995121510cdaa99c356), unmodified.
`LICENSE.dr_mp3` is its licence statement.

Only its low-level push decoder is used: `src/mp3.cc` includes it with stdio configured
out, float output on, Layer I and II out, its asserts off, its sync search wanting four
agreeing frames rather than ten, and its fallback allocator replaced by one that returns
nothing (the push decoder allocates nothing).

Upgrading is replacing a file and the commit and hash above, then running the module's
tests (`kormake test hifi_codec`).
