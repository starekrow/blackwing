-- The equalizer: a stage on hifi's stage interface. Biquads, presets, pre-attenuation so a
-- preset cannot clip. Volume is here too until it has a better home.
module "hifi_eq" {
  deps = { "core", "math", "hifi" },
  test_deps = { "hifi_platform", "hifi_codec" },
}
