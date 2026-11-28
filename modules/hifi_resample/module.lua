-- The resampler: one rate to another, as exactly as f64 allows. Polyphase windowed sinc
-- at the exact rational ratio of the two rates, a Kaiser window, its passband to 0.4535 of
-- the lower rate (20 kHz at 44.1) and its stopband from half of it. Plain code over memory
-- it is handed, as a decoder or a stage is: hifi runs it between the decoder and the
-- source ring when a device will not take a file's rate.
module "hifi_resample" {
  deps = { "core" },
}
