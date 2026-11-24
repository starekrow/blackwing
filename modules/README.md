# Modules

Blackwing's modules, each a directory with a `module.lua`. `hifi`, `hifi_codec`,
`hifi_platform`, `blackwing_player` and `blackwing` have code (slice 1), `hifi_eq`
(slice 2), `blackwing_tags` and `blackwing_library` (slice 3), and `ui_dev`,
`blackwing_window`, `blackwing_glue`, `blackwing_session` and `blackwing_script` (slice 4); the rest are skeletons. The
[master PRD](../docs/PRDs/Blackwing-Master-PRD.md) says what each is for and when.

| module | what |
|---|---|
| [hifi](hifi/README.md) | the hard-realtime audio path and its interfaces |
| [hifi_codec](hifi_codec/README.md) | the decoder contract and the decoders |
| [hifi_eq](hifi_eq/README.md) | the chain's stages, on hifi's interface: volume, and the equalizer |
| [hifi_platform](hifi_platform/README.md) | the output device, per OS |
| [media_platform](media_platform/README.md) | media keys, now-playing, keep-awake, per OS |
| [blackwing_player](blackwing_player/README.md) | the player: queue, transport, and an API of events and tables |
| [blackwing_tags](blackwing_tags/README.md) | tags from ranked sources, read-only, with sidecar overrides |
| [blackwing_library](blackwing_library/README.md) | roots, the scanner, the index, queries |
| [ui_dev](ui_dev/README.md) | the development face |
| [ui_wsz](ui_wsz/README.md) | the classic face: Winamp `.wsz` skins |
| [ui_clean](ui_clean/README.md) | the clean face |
| [ui_compact](ui_compact/README.md) | the clean face, compact |
| [blackwing_glue](blackwing_glue/README.md) | where the player and the library hear each other: a file queued is scanned next, what the library collects is queued |
| [blackwing_session](blackwing_session/README.md) | the session: the queue, the place, the chain and a face's part, saved when they change and restored paused |
| [blackwing_script](blackwing_script/README.md) | the script host: Lua over the player's and the library's API; `blackwing run` |
| [blackwing_window](blackwing_window/README.md) | what every face shares of the window: closing it ends the run |
| [blackwing](blackwing/README.md) | the application: the executable around the player, the library and a face |

```
ui_dev  ui_wsz  ui_clean  ui_compact            media_platform
    \      |       |        /                         |
     events and tables on Korvid's busses ------------+
        /                        \
blackwing_player            blackwing_library
        |        \                 |
        |       hifi_eq       blackwing_tags
        |        /
      hifi
      /   \
hifi_codec  hifi_platform
```

A module that a script module can reach has a `SCRIPT.md` beside its README: what it
shows a script, by name. They hang from
[the script-module spec](../docs/specs/script-modules.md), a draft.

Three layers. `hifi*` is the audio path and knows nothing of music collections or
players. `blackwing_player` is a player with no face and no library, which another
project could link. `blackwing`, the library, the glue between the two and the faces are
the application around it. The faces depend on the player and the library for their
declarations only.
