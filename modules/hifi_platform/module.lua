-- The output device, per OS: enumerate, open at a format, exclusive where the OS has it,
-- hot-plug, reported latency, the realtime thread. Not platform_audio extended: that one
-- opens the default device through SDL3 and offers none of this.
module "hifi_platform" {
  deps = { "core", "platform" },
}
