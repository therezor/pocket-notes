# TinyDecide device engine: WordPiece tokenizer, bidirectional Q4 encoder, choice head.
# The model files live in the repo's model/ folder (model.bin from tinydecide, vocab.bin from
# host/pack_model.mjs); model_data.S pulls them in with .incbin through the include path.
idf_component_register(
    SRCS
        engine.cpp
        dot_q4_pie.S
        model_data.S
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS .
    PRIV_REQUIRES esp_timer
)
# .incbin searches the assembler's own include path, which the compiler -I flags do not reach.
get_filename_component(TD_MODEL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../model" ABSOLUTE)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wa,-I${TD_MODEL_DIR}")
# Rebuild the embedded data when the model files change.
set_property(SOURCE model_data.S APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/../../../model/model.bin
    ${CMAKE_CURRENT_LIST_DIR}/../../../model/vocab.bin)
