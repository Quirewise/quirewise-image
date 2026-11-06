# Vendored source record: stb_image

- Component: `stb_image.h`, single-header image decoder.
- Version: **v2.30** (stated on line 1 of the header,
  `/* stb_image - v2.30 - public domain image loader */`).
- Origin: <https://github.com/nothings/stb> (Sean Barrett / nothings.org).
- License: **public domain / MIT dual** — the header states public domain, and
  the upstream repository offers MIT. No attribution requirement is triggered;
  the copyright notice in the header is retained.
- Why vendored: Quirewise Image uses it as an independent JPEG/PNG/BMP/GIF/HDR
  decoder for formats where the internal decoders are not yet complete. It is
  compiled by `src/formats/image/stb_image_impl.c` with only JPEG, PNG, BMP,
  GIF and HDR enabled, and with stdio and linear conversion disabled.
- Patches: none. The wrapper (`stb_image_impl.c`) only defines the feature
  macros; the header is byte-identical to upstream v2.30.
- Ownership: third-party code, private to the package (`third_party/stb` is an
  include directory only), never installed, never exposed by a public header.
- Consumers: `Quirewise::Image` (compiled) and `Quirewise::PDG`
  (`pdg_render.c` includes the same reviewed header, own copy under
  `third_party/stb`). Page's unused copy was removed in `3b302ef`.
