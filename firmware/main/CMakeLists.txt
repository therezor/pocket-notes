idf_component_register(
    SRCS
        main.cpp
        ui.cpp
        keyboard/Keyboard.cpp
        keyboard/KeyboardReader/IOMatrix.cpp
        keyboard/KeyboardReader/TCA8418.cpp
        keyboard/Adafruit_TCA8418/Adafruit_TCA8418.cpp
    INCLUDE_DIRS .
    REQUIRES tinydecide
    PRIV_REQUIRES esp_timer driver sdmmc
)
