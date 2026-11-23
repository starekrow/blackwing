# hifi_eq

The equalizer, as a stage on `hifi`'s stage interface: parametric bands, presets, and
pre-attenuation by the largest boost so a preset cannot clip. It is the first stage
written against the interface, and the control that proves the recut.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- Biquad bands with matched designs near Nyquist.
- Presets, as data.
- Coefficients from the engine's near-exact transcendentals, so a typed value and a
  dragged one give the same filter.
- What it declares to `hifi.chain`: flat is a pass-through.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
