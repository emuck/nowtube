idf_component_register(
    SRCS "i2s_probe.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s driver freertos esp_hw_support
)
