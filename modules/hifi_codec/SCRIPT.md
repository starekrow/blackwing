# hifi_codec — the script surface

Surface `hifi.decoder`, version 0.1.0 · draft, with [the spec](../../docs/specs/script-modules.md)

What a script module sees of the codecs: the decoder contract and the file as a sparse
span. A script decoder is the `DecoderDesc` of `korvid/hifi_codec/decoder.h` written in
Lua: it turns the bytes of one file into PCM, sees the bytes it is handed and nothing
else, and waits by returning. It is also how a module makes sound of its own
([Sound from nothing](#sound-from-nothing)). How decoding and the sparse file work is in
[hifi_codec's page](README.md).

Needs the capability `hifi.decoder`.

## A decoder

A file named in the module record's `decoders`, returning one record:

```lua
-- decoder.lua: 16-bit little-endian PCM with a 16-byte header of its own.
local S = kv.enum.DecodeStatus

return {
  name         = "rawpcm",
  memory_bytes = 1 << 20,

  probe = function(head) return head:sub(1, 4) == "RPCM" end,

  open = function(file)
    if not file:has(0, 16) then return { need = { at = 0, len = 16 } } end
    local rate, channels, frames = string.unpack("<I4I2xxI8", file:bytes(4, 16))
    if channels == 0 then return { reason = "no channels" } end
    return { state = { file = file, at = 0, frames = frames, channels = channels },
             info  = { format = { rate = rate, channels = channels, bits = 16 },
                       frames = frames } }
  end,

  decode = function(s, out, frames) ... return { frames = n, status = S.Ok } end,
  seek   = function(s, frame) s.at = frame; return { status = kv.enum.SeekStatus.Ok } end,
  close  = function(s) end,
}
```

| field | native | what |
|---|---|---|
| `name` | `DecoderDesc::name` | the kind of file, in a word |
| `memory_bytes` | `memory_bytes` | the block the decoder's whole Lua state lives in. 16 MB at most |
| `probe(head)` | `probe` | whether `head`, the file's first bytes as a string, starts a file of this kind. 64 bytes are enough to say no; `head` runs past any ID3v2 tags in front, as `decoder_head_bytes` says |
| `open(file)` | `open` | reads the headers and sets up to decode from the first frame. Returns a `DecoderOpen`, below |
| `decode(state, out, frames)` | `decode` | decodes up to `frames` frames into `out`, a `kv.frames` buffer of integers at the file's depth. Returns a `DecodeResult` |
| `seek(state, frame)` | `seek` | makes the next `decode` return frame `frame`. Returns a `SeekResult` |
| `close(state)` | `close` | the decoder's last call |

What they return, as tables with the native records' field names:

| record | fields |
|---|---|
| `DecoderOpen` | `state` (any Lua value) and `info`; or `need = { at, len }` to wait, then `open` again from the start; or `reason`, a string, on failure |
| `DecoderInfo` | `format = { rate, channels, bits }`; `frames` (0: the file does not say); `delay`, `padding` (gapless: frames at the ends that are not music); `estimated` boolean |
| `DecodeResult` | `frames` written; `status`, a `DecodeStatus`; `reason` with `Damaged`; `need = { at, len }` with `Wait` |
| `SeekResult` | `status`, a `SeekStatus`; `reason` with `Failed`; `need` with `Wait` |

`bits` is 1 to 32, and each sample is an integer in [-2^(bits-1), 2^(bits-1)).

The rules of the contract are the native ones (`decoder.h`): fewer frames than asked
only with `End`, `Damaged` or `Wait`; after `End` or `Damaged`, 0 frames and the same
status until a seek; a `Wait` changes nothing but the frames it returned; a seek that
waits or fails leaves the decoder where it was.

## The file

`open` is handed the file as an object over a `SparseSpan` (`korvid/hifi_codec/sparse.h`):
its whole length, of which only some pieces have arrived.

| call | native | what |
|---|---|---|
| `file:size()` | `size` | the file's length in bytes |
| `file:has(at, len)` | `sparse_has` | whether every byte of the range that is inside the file is there |
| `file:missing(at, len)` | `sparse_missing` | `at, len` of what must still arrive before the range can be read; `len` 0 when it all can |
| `file:bytes(at, len)` | reading `bytes` | the range as a string; a Lua error if any of it is not there |
| `file:piece_bytes()` | `1 << piece_shift` | what the file is read in |

Offsets count from 0, as the file's do. `string.unpack` takes a `file:bytes` string
apart. A decoder asks for what it needs by returning `need`, and is called again when it
has come; it never blocks. Pieces it has gone past may be given back, so a decoder keeps
what it needs from them, not the offsets.

A decoder never sees the file's path or name.

## Where it runs

As a stage does ([hifi](../hifi/SCRIPT.md#where-it-runs)): each open file is a Lua state
of its own inside `memory_bytes`, with the base library, `string`, `table`, `math`,
`kv.frames` and `kv.log`, and no `ctx`. `decode` runs on hifi's render thread, ahead of
the playhead; `probe` runs in a state of its own, made and dropped for the call.

## Sound from nothing

A decoder with `synth = true` in its record needs no file. It is how a script module
produces audio of its own: a tone, a generated piece, a test signal.

```lua
return {
  name = "sine", synth = true, memory_bytes = 1 << 18,
  open = function(file, args)             -- file:size() == 0; args: "hz=440&seconds=5"
    return { state = { n = 0, hz = 440 },
             info  = { format = { rate = 48000, channels = 2, bits = 24 }, frames = 240000 } }
  end,
  decode = function(s, out, frames) ... end,
  seek   = function(s, frame) s.n = frame; return { status = kv.enum.SeekStatus.Ok } end,
  close  = function(s) end,
}
```

It is queued by its module's control state, with the capability `player.queue`:

```lua
ctx:emit("PlayerEnqueue", { path = "script:tones/sine?hz=440&seconds=5" })
```

The path `script:<module>/<decoder>?<args>` names a decoder, not a file. A module may
enqueue its own; the row shows in `player.queue` with that path and `size` 0. `probe` is
not called. `frames` 0 in `info` means it plays until it returns `End`, which may be
never: the user skips it.

**Draft choice.** The path form keeps `PlayerEnqueue` as it is. It costs the player one
branch where it opens a row.

## Behaviour worth knowing

**Linked decoders are asked first.** A script decoder is offered a file only when no
linked one's `probe` took it, in the order the modules were enabled.

**The answers are checked.** `frames` at most what was asked; a format hifi plays; a
`need` inside the file. An answer that fails ends the source `Damaged`, or `Failed` if it
had not opened.

**Deadlines and errors** are a stage's: over a quarter of the block's duration, or a Lua
error, ends the source `Damaged` with the reason. The player goes on to the next item.

**Exactness.** A decoder's integers are "the file's samples" for `player.output.exact`,
as a linked one's are. Nothing checks them against anything.

**Malformed files are the cost.** A decoder reads hostile bytes. It runs inside its
block and its budget whatever they say, and the worst it can do is end the source.

## Enums

| enum | values |
|---|---|
| `DecodeStatus` | `Ok` 0, `End` 1, `Damaged` 2, `Wait` 3 |
| `SeekStatus` | `Ok` 0, `Wait` 1, `Failed` 2 |

## Not here

- `decoders()`, `decoder_for(head)`, `decoder_head_bytes(first, size)`: the player's way
  of finding a decoder. A script provides one; it does not look one up.
- The linked decoders (`flac.h`, `mp3.h`). A script cannot call them.

## What hifi_codec changes for this

Nothing. The player asks the script host's decoders after `decoder_for` says no, and
routes a `script:` path to its decoder with an empty file.
