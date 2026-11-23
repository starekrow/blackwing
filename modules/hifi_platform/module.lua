-- The output device, per OS: open at a format, a callback on the backend's realtime
-- thread, the delay to the listener's ear. Not platform_audio extended: that one opens the
-- default device through SDL3 and offers none of this. The capture device (no hardware; it
-- keeps what it is given) is here too, and is what every test plays into.
--
-- Linux plays through PipeWire (src/linux/pipewire.cc), an external this module defines,
-- since the engine does not use it.
module "hifi_platform" {
  deps      = { "core" },
  external  = { pipewire = { ">=0.3.48", platforms = { "linux" } } },
  externals = {
    linux = {
      pipewire = {
        pkg_config = "libpipewire-0.3",
        include    = { "/usr/include/pipewire-0.3", "/usr/include/spa-0.2" },
        libs       = { "pipewire-0.3" },
        fetch      = "sudo apt install libpipewire-0.3-dev",
      },
    },
  },
}
