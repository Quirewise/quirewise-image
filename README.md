# quirewise-image

Standalone Quirewise image library.

This repository owns still-image probing and decoding, image-folder enumeration,
and bounded grayscale scan processing. It can be consumed without the Quirewise
application repository.

## Build

```powershell
cmake -S . -B build
cmake --build build
cmake --install build --prefix install
```

The umbrella target is `Quirewise::Image`. Public headers are under
`include/quirewise/`.
