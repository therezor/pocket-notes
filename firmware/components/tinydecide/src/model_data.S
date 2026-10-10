// Embeds model.bin and vocab.bin in flash (.rodata, MMU-mapped) when the ESP-IDF component is built
// with CONFIG_TINYDECIDE_EMBED_MODEL (CMakeLists.txt defines TINYDECIDE_EMBED and the include path).
// The PIE kernel loads 128-bit words, so the model must start on a 16-byte boundary; all Q4 offsets
// inside it are multiples of 16 (checked by tools/pack.mjs). It starts on a 64-byte boundary so the
// weight rows, at 64-byte offsets in model.bin, line up with the flash cache lines: unaligned, each
// row touches one more line and the matmuls run up to ~10% slower.
#if defined(TINYDECIDE_EMBED)
    .section .rodata.td_model, "a"
    .balign 64
    .global td_model_bin
td_model_bin:
    .incbin "model.bin"
    .global td_model_bin_end
td_model_bin_end:
    .balign 16
    .global td_vocab_bin
td_vocab_bin:
    .incbin "vocab.bin"
    .global td_vocab_bin_end
td_vocab_bin_end:
#endif
