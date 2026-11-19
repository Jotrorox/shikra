# bench/ext -- the (shikra ...) extension libraries on two small kernels

Each kernel exists twice: a portable R7RS program, and a variant that uses the extension libraries
behind `cond-expand` on the `shikra` feature (it also runs unchanged on other R7RS systems; checked
with Gambit). Run: `shikra build bench/ext/NAME.scm -o x && ./x [size] [repetitions]`.

| kernel | portable | with extensions | what changes |
|---|---|---|---|
| `matmul` (400x400, 5 times) | vectors of boxed flonums, generic `+ *` | `(shikra f64vector)` + `(shikra flonum)` + `(shikra fixnum)` | raw doubles in memory and in registers |
| `vhash` (10^6 elements, 200 times) | `+ * modulo odd?` on values taken out of a vector | `fx+ fx* fxand fxodd?` | one fixnum check per element where it enters |

Timings and the emitted inner loops: DESIGN.md "Extensions as built".
