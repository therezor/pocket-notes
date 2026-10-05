# TinyDecide device engine (M1): WordPiece tokenizer, bidirectional Q4 encoder,
# choice/noul/score/span heads, corrections. Reusable by other Cardputer demos.
idf_component_register(
    SRCS
        dot_q4_pie.S
    INCLUDE_DIRS include
)
