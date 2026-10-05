// Embeds model/model.bin and model/vocab.bin in flash (.rodata, MMU-mapped). The PIE kernel loads
// 128-bit words, so the model starts on a 16-byte boundary; all Q4 offsets inside it are multiples
// of 16 (checked by host/pack_model.mjs). The files are found through the component's include path.
    .section .rodata.td_model, "a"
    .balign 16
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
