# QR Code generator

This directory vendors the C implementation of Project Nayuki's QR Code
generator at commit `2c9044de6b049ca25cb3cd1649ed7e27aa055138`.

Tilefinch uses it only for the user-triggered diagnostic export screen. The
encoder is allocation-free; Tilefinch fixes the symbol to QR Version 27,
medium error correction, and renders modules as exact 2-by-2 pixel blocks.

Upstream: <https://github.com/nayuki/QR-Code-generator>

The source is distributed under the MIT License reproduced in the header of
both vendored source files.
