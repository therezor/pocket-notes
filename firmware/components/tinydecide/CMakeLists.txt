# TinyDecide ESP-IDF component: WordPiece tokenizer, Q4 encoder, the four answer heads.
# With CONFIG_TINYDECIDE_EMBED_MODEL, model.bin and vocab.bin are pulled into the app image by
# src/model_data.S (.incbin). vocab.bin ships in model/; model.bin is looked up in
# CONFIG_TINYDECIDE_MODEL_DIR, then model/, then the Hugging Face repo root (../..).
set(td_srcs src/engine.cpp src/dot_q4_pie.S)
set(td_model "")
if(CONFIG_TINYDECIDE_EMBED_MODEL)
    idf_build_get_property(td_project_dir PROJECT_DIR)
    set(td_dirs "${CMAKE_CURRENT_LIST_DIR}/model" "${CMAKE_CURRENT_LIST_DIR}/../..")
    if(NOT "${CONFIG_TINYDECIDE_MODEL_DIR}" STREQUAL "")
        get_filename_component(td_cfg_dir "${CONFIG_TINYDECIDE_MODEL_DIR}" ABSOLUTE BASE_DIR "${td_project_dir}")
        set(td_dirs "${td_cfg_dir}")
    endif()
    foreach(d ${td_dirs})
        if(td_model STREQUAL "" AND EXISTS "${d}/model.bin")
            get_filename_component(td_model "${d}/model.bin" ABSOLUTE)
        endif()
    endforeach()
    if(td_model STREQUAL "")
        message(FATAL_ERROR "TinyDecide: model.bin not found in ${td_dirs}. Download it from "
                            "https://huggingface.co/TheREZOR/TinyDecide, or turn off CONFIG_TINYDECIDE_EMBED_MODEL.")
    endif()
    # The engine's offsets (src/td_meta.h) belong to one model.bin; refuse any other at build time.
    file(READ "${CMAKE_CURRENT_LIST_DIR}/src/td_meta.h" td_meta)
    string(REGEX MATCH "MODEL_SHA256 = \"([0-9a-f]+)\"" td_m "${td_meta}")
    set(td_want "${CMAKE_MATCH_1}")
    file(SHA256 "${td_model}" td_got)
    if(NOT td_got STREQUAL td_want)
        message(FATAL_ERROR "TinyDecide: ${td_model} is not the build src/td_meta.h was generated for "
                            "(sha256 ${td_got}, want ${td_want}). Run tools/pack.mjs on it.")
    endif()
    list(APPEND td_srcs src/model_data.S)
endif()

idf_component_register(
    SRCS ${td_srcs}
    INCLUDE_DIRS src
    PRIV_REQUIRES esp_timer esp_partition spi_flash
)

if(td_model)
    get_filename_component(td_model_dir "${td_model}" DIRECTORY)
    # .incbin searches the assembler's include path, which the compiler -I flags do not reach.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TINYDECIDE_EMBED=1)
    target_compile_options(${COMPONENT_LIB} PRIVATE "-Wa,-I${td_model_dir}" "-Wa,-I${CMAKE_CURRENT_LIST_DIR}/model")
    set_property(SOURCE src/model_data.S APPEND PROPERTY OBJECT_DEPENDS
        "${td_model}" "${CMAKE_CURRENT_LIST_DIR}/model/vocab.bin")
endif()
# Inference hot loops want -O2 even in a size-optimised app.
set_source_files_properties(src/engine.cpp PROPERTIES COMPILE_OPTIONS "-O2")
