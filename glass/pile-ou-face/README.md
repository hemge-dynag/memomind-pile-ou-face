# Pile ou Face

Standalone glasses app (no phone needed): a quick upward head motion tosses a
coin. It flies up over a row of background trees, tumbles between its face
and its edge, falls back down, and lands on PILE or FACE.

- Trigger: IMU "head raise" gesture (`GM_PLUGIN_IMU_GESTURE_HEAD_RAISE`).
- Outcome: picked via the Host's Random extension (`GM_PLUGIN_EXTENSION_RANDOM`).
- Coin: two real engraved faces (indexed 4-bit, 16-shade grayscale bitmaps,
  generated at runtime sized to the actual screen) — a reeded rim (16
  alternating ridge wedges drawn with pure integer angle buckets, no
  trigonometry) around a frontal portrait (head, hairline, eyes, nose, mouth)
  for FACE and a "10€" value mark for PILE (hand-drawn via a tiny 5x7 pixel
  font blitted straight into the bitmap, the € rendered as a barred C), plus
  a fluted edge-on sliver — swapped via `image_set_source` to sell the
  tumble. The result is also announced in the title text, and the coin
  visually lands on the matching face.
- Flight: the coin tosses from the bottom-right corner to the bottom-left
  corner — x glides linearly between the two while y rises and falls along
  an integer fixed-point parabola (`4*p*(1-p)`, no libm needed), tracing a
  circular-looking arc over ~1.7s and landing exactly at the left spot as
  the last of 10 decelerating spin ticks completes. The amplitude is sized
  so the peak, at mid-arc, puts the coin right at the top of the screen
  (leaving only its own radius as clearance) — high above the conifers'
  tops, which stay the fixed reference that sells the height of the throw.
- Background: four conifers (dark trunk + three tiered triangular tops,
  two size variants for a little depth), generated at runtime proportional
  to the real screen size, created first so LVGL's creation-order
  z-layering puts them behind the coin and text.
- While spinning, a new head-raise is ignored; once a result is shown,
  head-raise again starts a new flip.

Note: all arithmetic is kept in 32-bit (no `int64_t`/`uint64_t` division) —
this freestanding RISC-V target does not link the 64-bit div/mod runtime
helpers (`__udivdi3`/`__divdi3`), so any accidental 64-bit math fails at
link time.

Build:

```sh
python3 build.py glass --force
```
