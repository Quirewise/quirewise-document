# quirewise-document

Standalone Quirewise document library: PDF structure, text extraction,
rasterisation and merge, plus the conversion dispatch shared by the application
packages.

CMake target: `Quirewise::Document`. The package owns the `convert.h` dispatch
declaration and depends on the runtime contracts through
`Quirewise::Runtime`; it never links the application.

Build with `cmake -S . -B build` and `cmake --build build`.

## Capabilities

- PDF structure: classic cross-reference tables, cross-reference streams and
  object streams, incremental updates, and RC4/AES crypt handlers.
- Text extraction and page rendering through one internal parser: no MuPDF
  code, header or library is involved.
- Merge of classic-xref documents.
- Image XObjects are rasterised with the filter matrix below; `/Decode [1 0]`
  is honoured for the bitonal filters and `/JBIG2Globals` is resolved through
  the object stream.
- Content is scaled by the requested resolution: the content matrix starts with
  `dpi / 72`, so a page rendered at 144 or 300 dpi keeps the same layout.

## PDF image filters

| Filter | Status |
|---|---|
| raw samples (Flate/LZW/RunLength, PNG and TIFF predictors) | supported |
| `/DCTDecode` (JPEG) | supported; colour space from the frame component count |
| `/JPXDecode` (JPEG 2000) | supported for JP2 containers and raw codestreams |
| `/CCITTFaxDecode` (Group 4) | supported, including the `/Decode [1 0]` polarity |
| `/JBIG2Decode` | generic regions (MMR or T.88 arithmetic, templates 0-3), symbol dictionaries with arithmetic-coded glyphs, text regions |

JBIG2 streams that use Huffman-coded dictionaries or text regions, refinement,
halftones, pattern dictionaries or a transposed text region return
`QW_STATUS_ERR_UNSUPPORTED` rather than drawing a partial page.

## Tests

`ctest` runs the PDF suites (structure, xref, encryption, text, merge, image
filters, incremental merge) plus render tests whose fixtures were cross-checked
against independent decoders:

- `pdf_ccitt_render_test` — Group 4 image XObject against a libtiff-generated
  strip;
- `pdf_jpx_render_test` — JP2 image XObject;
- `pdf_jbig2_render_test` — MMR generic region, both polarities;
- `pdf_jbig2_arith_render_test` — arithmetic generic region with the standalone
  file header, plus 144 dpi content scaling;
- `pdf_jbig2_symbol_render_test` — symbol dictionary and text region.

The JBIG2 fixtures were decoded by `jbig2dec` and Ghostscript before being
pinned; the 2550x3300 `perf_codecs_jbig2` page and the 128x96 file-header page
from the pdf.js corpus match those decoders pixel for pixel.
