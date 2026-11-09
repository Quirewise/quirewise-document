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
| `/JBIG2Decode` | generic regions with MMR coding; arithmetic-coded regions, symbol dictionaries, text regions, halftones and refinement return `QW_STATUS_ERR_UNSUPPORTED` instead of drawing a partial page |

`/Decode [1 0]` is honoured for the bitonal filters, `/JBIG2Globals` is
resolved through the object stream, and images are rasterised with the correct
orientation.

## Tests

`ctest` runs the PDF suites (structure, xref, encryption, text, merge, image
filters) plus render tests for CCITT Group 4, JPEG 2000 and JBIG2 streams whose
data was cross-checked against independent decoders.