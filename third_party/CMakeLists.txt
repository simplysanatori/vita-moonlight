add_library(enet STATIC
    enet/callbacks.c
    enet/compress.c
    enet/host.c
    enet/list.c
    enet/packet.c
    enet/peer.c
    enet/protocol.c
    enet/unix.c
)

target_include_directories(enet PUBLIC enet/include)

add_library(h264bitstream STATIC
    h264bitstream/h264_nal.c
    h264bitstream/h264_sei.c
    h264bitstream/h264_stream.c
)
target_include_directories(h264bitstream PUBLIC h264bitstream)

add_library(inih STATIC
    inih/ini.c
)
target_include_directories(inih PUBLIC inih)

add_library(libuuid STATIC
    libuuid/src/clear.c
    libuuid/src/compare.c
    libuuid/src/copy.c
    libuuid/src/gen_uuid.c
    libuuid/src/isnull.c
    libuuid/src/pack.c
    libuuid/src/parse.c
    libuuid/src/unpack.c
    libuuid/src/unparse.c
    libuuid/src/uuid_time.c
)
target_include_directories(libuuid PUBLIC libuuid/src)

add_library(moonlight-common-c STATIC
    moonlight-common-c/src/AudioStream.c
    moonlight-common-c/src/ByteBuffer.c
    moonlight-common-c/src/Connection.c
    moonlight-common-c/src/ConnectionTester.c
    moonlight-common-c/src/ControlStream.c
    moonlight-common-c/src/FakeCallbacks.c
    moonlight-common-c/src/InputStream.c
    moonlight-common-c/src/LinkedBlockingQueue.c
    moonlight-common-c/src/Misc.c
    moonlight-common-c/src/Platform.c
    moonlight-common-c/src/PlatformCrypto.c
    moonlight-common-c/src/PlatformSockets.c
    moonlight-common-c/src/RecorderCallbacks.c
    moonlight-common-c/src/RtpAudioQueue.c
    moonlight-common-c/src/RtpVideoQueue.c
    moonlight-common-c/src/RtspConnection.c
    moonlight-common-c/src/RtspParser.c
    moonlight-common-c/src/SdpGenerator.c
    moonlight-common-c/src/SimpleStun.c
    moonlight-common-c/src/VideoDepacketizer.c
    moonlight-common-c/src/VideoStream.c
    # Reed-Solomon FEC: nanors (git submodule of moonlight-common-c, so the clone must be --recursive).
    # NEON is selected at compile time inside oblas_lite.c when __ARM_NEON is defined (there is no
    # run-time probe on 32-bit ARM, and the scalar fallback is silent). The vitasdk GCC defines it
    # by default (--with-arch=armv7-a --with-fpu=neon --with-float=hard); CI checks that it did.
    moonlight-common-c/nanors/rs.c
    moonlight-common-c/nanors/deps/obl/oblas_common.c
    moonlight-common-c/nanors/deps/obl/oblas_lite.c
)
target_include_directories(moonlight-common-c PUBLIC moonlight-common-c/src)
target_include_directories(moonlight-common-c PRIVATE
    moonlight-common-c/nanors
    moonlight-common-c/nanors/deps
    moonlight-common-c/nanors/deps/obl
)
# The app builds with -std=gnu99, but upstream moonlight-common-c and nanors are C11. nanors'
# oblas_common.c falls back to aligned_alloc() on non-Linux targets, which newlib only declares
# for C11 and later; under gnu99 it is an implicit declaration, an error since GCC 14.
target_compile_options(moonlight-common-c PRIVATE -std=gnu11)

add_library(mdnsniff STATIC
    mdnsniff/udp_sniffer_vita.c
)
target_include_directories(mdnsniff PUBLIC mdnsniff)
