# Deps

function(gx_add_libchdr target GX_ROOT)
  set(CHDLIBDIR "${GX_ROOT}/core/cd_hw/libchdr")

  add_library(${target} STATIC)

  target_sources(${target} PRIVATE
    "${CHDLIBDIR}/src/libchdr_bitstream.c"
    "${CHDLIBDIR}/src/libchdr_cdrom.c"
    "${CHDLIBDIR}/src/libchdr_chd.c"
    "${CHDLIBDIR}/src/libchdr_flac.c"
    "${CHDLIBDIR}/src/libchdr_huffman.c"

    "${CHDLIBDIR}/deps/lzma-24.05/src/LzFind.c"
    "${CHDLIBDIR}/deps/lzma-24.05/src/LzmaEnc.c"
    "${CHDLIBDIR}/deps/lzma-24.05/src/LzmaDec.c"
    "${CHDLIBDIR}/deps/lzma-24.05/src/CpuArch.c"

    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/decompress/zstd_decompress.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/decompress/huf_decompress.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/decompress/zstd_decompress_block.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/decompress/zstd_ddict.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/common/entropy_common.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/common/error_private.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/common/fse_decompress.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/common/xxhash.c"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib/common/zstd_common.c")

  target_include_directories(${target} PUBLIC
    "${CHDLIBDIR}/include"
    "${CHDLIBDIR}/deps/zstd-1.5.6/lib"
    "${CHDLIBDIR}/deps/zstd-1.5.6/common"
    "${CHDLIBDIR}/deps/lzma-24.05/include"
    "${CHDLIBDIR}/deps/zlib-1.3.1"
  )

  target_compile_definitions(${target} PUBLIC
    _CRT_SECURE_NO_WARNINGS
    LSB_FIRST
    USE_16BPP_RENDERING
    MAXROMSIZE=33554432
    HAVE_YM3438_CORE
    Z7_ST
    _7ZIP_ST
    FLAC__NO_DLL
    FLAC__HAS_OGG=0
    $<$<BOOL:${GX_HOOK_CPU}>:HOOK_CPU>
    $<$<AND:$<BOOL:${GX_ENABLE_CHD}>,$<PLATFORM_ID:Windows>>:HAVE_FSEEKO>
  )

  target_compile_definitions(${target} PRIVATE INLINE=static inline)
endfunction()