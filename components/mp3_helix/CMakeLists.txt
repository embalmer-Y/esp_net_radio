# mp3_helix: libhelix-mp3 定点 MP3 解码器(vendored)
#
# 来源: chmorgan/esp-libhelix-mp3 v1.0.3 所附源码(RCSL/RPSL 双许可, 允许再分发,
#       许可文本见本目录 LICENSE.txt/RCSL.txt/RPSL.txt; 上游为 RealNetworks Helix)。
#
# 为什么 vendor 而不用 managed 组件(2026-11-09, M1.5 攻坚结论):
#   - esp_audio_codec 的 MP3 即闭源 minimp3 派生, S31 上慢且吞帧;
#   - managed 组件以 -Os 编译, 实测 44.1k/2ch 解码 25-26ms/帧(≈1.0x 实时, 零余量);
#   - 本地以 -O3 编译同源码, 可自由控制优化级别(见 docs/01 M1.5 小节)。
idf_component_register(
    SRCS "mp3dec.c" "mp3tabs.c"
         "real/bitstream.c" "real/buffers.c" "real/dct32.c" "real/dequant.c"
         "real/dqchan.c" "real/huffman.c" "real/hufftabs.c" "real/imdct.c"
         "real/polyphase.c" "real/scalfact.c" "real/stproc.c" "real/subband.c"
         "real/trigtabs.c"
    INCLUDE_DIRS "pub"
    LDFRAGMENTS "linker.lf"
)

# -O2 + 全组件 IRAM 常驻(见 linker.lf 注释): -O3 实测无收益(代码膨胀反而
# 增大取指压力), IRAM 常驻才是根治; 若后续 IRAM 紧张可只映射热点文件
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
