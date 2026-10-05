set(srcs "main.c" "settings.c" "lcd.c" "font.c" "net.c" "dns.c" "web.c" "stratum.c" "miner.c" "stats.c" "ui.c"
         "sha256.c" "sha_hw.c" "sha_hw_scan.S" "work.c")
if(SHA_BENCH)
    list(APPEND srcs "bench.c")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "." EMBED_TXTFILES "web/index.html")
if(SHA_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHA_BENCH=${SHA_BENCH})
endif()
