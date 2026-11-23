-- The output device, per OS: open at a format, a callback on the backend's realtime
-- thread, the delay to the listener's ear. Not platform_audio extended: that one opens the
-- default device through SDL3 and offers none of this. The capture device (no hardware; it
-- keeps what it is given) is here too, and is what every test plays into.
module "hifi_platform" {
  deps = { "core" },
}
