file(GLOB FONT_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.c")

idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "gfx.c"
        "font.c"
        "button.c"
        "settings.c"
        "library.c"
        "reader.c"
        "ui.c"
        "portal.c"
        "convert.c"
        "textout.c"
        "html.c"
        "epub.c"
        "pdftext.c"
        "zip.c"
        "inflate.c"
        "qrcode.c"
        ${FONT_SRCS}
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES
        esp_driver_spi esp_driver_gpio esp_driver_ledc
        nvs_flash fatfs wear_levelling vfs
        esp_wifi esp_netif esp_event esp_http_server
        esp_timer lwip
)

# long file names are truncated on purpose when copied into fixed buffers
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
