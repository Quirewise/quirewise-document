# quirewise-document

Standalone Quirewise document library.

CMake target: `Quirewise::Document`.

Build with `cmake -S . -B build` and `cmake --build build`.

## PDF image filters

The rasteriser decodes these image XObject filters:

| Filter | Status |
|---|---|
| raw samples (Flate/LZW/RunLength, predictors) | supported |
| `/DCTDecode` (JPEG) | supported |
| `/JPXDecode` (JPEG 2000) | supported, JP2 containers and raw codestreams |
| `/CCITTFaxDecode` (Group 4) | supported |
| `/JBIG2Decode` | generic regions (MMR or T.88 arithmetic, templates 0-3), symbol dictionaries with arithmetic-coded glyphs, text regions; Huffman-coded dictionaries, refinement, halftones and pattern dictionaries return `QW_STATUS_ERR_UNSUPPORTED` instead of drawing a partial page |

`/Decode [1 0]` is honoured for the bitonal filters, `/JBIG2Globals` is
resolved through the object stream, and content is scaled by the requested
resolution (72 scale factor applied to the content matrix).

## Tests

`ctest` runs the PDF suites (structure, xref, encryption, text, merge, image
filters) plus render tests for CCITT Group 4, JPEG 2000 and JBIG2 streams whose
data was cross-checked against jbig2dec and Ghostscript: MMR generic regions,
arithmetic generic regions with and without the standalone file header, and a
symbol dictionary with a text region.