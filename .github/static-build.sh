#!/bin/bash
#
# Builds cyanrip with all third-party libraries linked statically, so the
# binary runs without any of them being installed. Used by the CI for the
# Linux (musl, fully static) and macOS (only system libraries and frameworks
# are linked dynamically) release binaries.
#
# The toolchain, meson, ninja, cmake, nasm, pkg-config and autotools must be
# installed. On Linux zlib, bzip2, openssl and libxml2 must be installed
# with their static libraries; on macOS these are built here or come from
# the SDK.

set -eo pipefail

CURL_VERSION=8.16.0
NEON_VERSION=0.37.1
LIBQRENCODE_VERSION=4.1.1
LIBCDIO_VERSION=2.4.0
LIBCDIO_PARANOIA_VERSION=10.2+2.0.2
LAME_VERSION=3.100
LIBOGG_VERSION=1.3.6
LIBVORBIS_VERSION=1.3.7
OPUS_VERSION=1.5.2
FFMPEG_VERSION=8.0
OPENSSL_VERSION=3.5.4
LIBXML2_VERSION=2.14.6
CHROMAPRINT_VERSION=1.6.1

buildCyanrip() {
    if [[ $CYANRIPOS == darwin ]]; then
        build_openssl
        build_libxml2
    fi
    build_curl
    build_neon
    build_libmusicbrainz
    build_libqrencode
    build_chromaprint
    build_libcdio
    build_libcdio_paranoia
    build_lame
    build_libogg
    build_libvorbis
    build_opus
    build_ffmpeg
    build_cyanrip
}

cyan_prepare() {
    CYANRIPREPODIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    CYANRIPBUILDDIR="$CYANRIPREPODIR/.github/_build"
    CYANRIPINSTALLDIR="$CYANRIPREPODIR/.github/_install"
    CYANRIPOS="$(uname -s | tr '[:upper:]' '[:lower:]')"
    CYANRIPJOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
    PKG_CONFIG_PATH="$CYANRIPINSTALLDIR/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    CPPFLAGS="-I$CYANRIPINSTALLDIR/include"
    CFLAGS="-O2 -pipe"
    CXXFLAGS="$CFLAGS"
    LDFLAGS="-L$CYANRIPINSTALLDIR/lib"
    export CYANRIPREPODIR CYANRIPBUILDDIR CYANRIPINSTALLDIR CYANRIPOS CYANRIPJOBS
    export PKG_CONFIG_PATH CPPFLAGS CFLAGS CXXFLAGS LDFLAGS
    # Nothing outside the install prefix and the system is allowed in, so
    # Homebrew libraries can't get picked up on macOS.
    unset PKG_CONFIG_LIBDIR
    mkdir -p "$CYANRIPBUILDDIR" "$CYANRIPINSTALLDIR"
}

# Downloads and unpacks a release tarball, then enters its directory.
cyan_do_tarball() {
    local url=$1 dir=$2 compress
    case $url in
        *.xz)  compress=J ;;
        *.bz2) compress=j ;;
        *)     compress=z ;;
    esac
    cd "$CYANRIPBUILDDIR"
    rm -rf "$dir"
    curl -fsSL --retry 5 "$url" | tar -x$compress
    cd "$dir"
}

# Clones a git tag or branch (default HEAD) with no history, then enters it.
cyan_do_vcs() {
    local url=$1 ref=$2 dir
    dir="$(basename "$url" .git)"
    cd "$CYANRIPBUILDDIR"
    rm -rf "$dir"
    git clone -q --depth 1 ${ref:+--branch "$ref"} "$url" "$dir"
    cd "$dir"
}

cyan_do_confmakeinstall() {
    ./configure --disable-shared --enable-static --prefix="$CYANRIPINSTALLDIR" "$@"
    make -j"$CYANRIPJOBS"
    make install
}

cyan_do_cmakeinstall() {
    cmake -B _build -G Ninja -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX="$CYANRIPINSTALLDIR" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$CYANRIPINSTALLDIR" \
        -DCMAKE_BUILD_TYPE=Release "$@"
    ninja -C _build
    ninja -C _build install
}

# macOS only: the SDK has no OpenSSL. /etc/ssl/cert.pem is where macOS keeps
# the system root certificates as a PEM bundle, so use it as the default.
build_openssl() {
    cyan_do_tarball "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" "openssl-$OPENSSL_VERSION"
    ./Configure no-shared no-tests no-docs no-apps \
        --prefix="$CYANRIPINSTALLDIR" --libdir=lib --openssldir=/etc/ssl
    make -j"$CYANRIPJOBS"
    make install_sw
}

# macOS only: the SDK's libxml2 has no pkg-config file, which neon and
# libmusicbrainz need to find it.
build_libxml2() {
    cyan_do_tarball "https://download.gnome.org/sources/libxml2/${LIBXML2_VERSION%.*}/libxml2-$LIBXML2_VERSION.tar.xz" "libxml2-$LIBXML2_VERSION"
    cyan_do_confmakeinstall --without-python --without-lzma --without-icu \
        --without-http --with-zlib --with-iconv
}

build_curl() {
    local ca_args
    # The system's CA store is used at runtime. macOS keeps one at a fixed
    # path; on Linux it differs per distribution, so leave it to OpenSSL's
    # default lookup, which a compiled-in but missing bundle path would
    # override with an error.
    if [[ $CYANRIPOS == darwin ]]; then
        ca_args="--with-ca-bundle=/etc/ssl/cert.pem --without-ca-path"
    else
        ca_args="--with-ca-fallback --without-ca-bundle --without-ca-path"
    fi

    cyan_do_tarball "https://github.com/curl/curl/releases/download/curl-${CURL_VERSION//./_}/curl-$CURL_VERSION.tar.xz" "curl-$CURL_VERSION"
    # Only HTTP(S) is needed.
    cyan_do_confmakeinstall --with-openssl $ca_args \
        --disable-{ldap,ldaps,rtsp,dict,telnet,tftp,pop3,imap,smb,smtp,gopher,mqtt,ftp,file} \
        --disable-{manual,docs,libcurl-option,ntlm,unix-sockets} \
        --without-{libpsl,libidn2,nghttp2,brotli,zstd,libssh2,librtmp,zlib}
}

build_neon() {
    cyan_do_vcs "https://github.com/notroj/neon.git" "$NEON_VERSION"
    ./autogen.sh
    ./configure --disable-shared --enable-static --prefix="$CYANRIPINSTALLDIR" \
        --disable-{nls,debug,webdav} --with-ssl=openssl --with-libxml2
    make -j"$CYANRIPJOBS"
    # A git checkout has no man pages, which "make install" insists on.
    make install-lib install-headers install-config
}

build_libmusicbrainz() {
    cyan_do_vcs "https://github.com/cyanreg/libmusicbrainz.git"
    cyan_do_cmakeinstall
}

build_libqrencode() {
    cyan_do_tarball "https://github.com/fukuchi/libqrencode/archive/refs/tags/v$LIBQRENCODE_VERSION.tar.gz" "libqrencode-$LIBQRENCODE_VERSION"
    cyan_do_cmakeinstall -DWITH_TOOLS=NO -DWITH_TESTS=NO -DWITHOUT_PNG=YES
}

build_chromaprint() {
    local cxxlib
    # The bundled kissfft is used rather than whichever FFT is found first,
    # so the library depends on nothing but the C++ runtime.
    cyan_do_tarball "https://github.com/acoustid/chromaprint/releases/download/v$CHROMAPRINT_VERSION/chromaprint-$CHROMAPRINT_VERSION.tar.gz" "chromaprint-$CHROMAPRINT_VERSION"
    cyan_do_cmakeinstall -DFFT_LIB=kissfft -DBUILD_TOOLS=OFF -DBUILD_TESTS=OFF
    # chromaprint is C++, but its pkg-config file doesn't say so, so linking
    # it statically into a C program would leave the C++ runtime out.
    if [[ $CYANRIPOS == darwin ]]; then
        cxxlib=-lc++
    else
        cxxlib=-lstdc++
    fi
    echo "Libs.private: $cxxlib -lm" >> "$CYANRIPINSTALLDIR/lib/pkgconfig/libchromaprint.pc"
}

build_libcdio() {
    # The macOS backend needs cdio_get_device_fd(), newer than the last
    # release on ftp.gnu.org.
    cyan_do_tarball "https://github.com/libcdio/libcdio/releases/download/$LIBCDIO_VERSION/libcdio-$LIBCDIO_VERSION.tar.gz" "libcdio-$LIBCDIO_VERSION"
    cyan_do_confmakeinstall --disable-{cxx,example-progs,cddb,vcd-info} \
        --without-{cd-drive,cd-info,cdda-player,cd-read,iso-info,iso-read}
}

build_libcdio_paranoia() {
    cyan_do_tarball "https://ftp.gnu.org/gnu/libcdio/libcdio-paranoia-$LIBCDIO_PARANOIA_VERSION.tar.bz2" "libcdio-paranoia-$LIBCDIO_PARANOIA_VERSION"
    cyan_do_confmakeinstall --disable-{cxx,example-progs}
}

build_lame() {
    cyan_do_tarball "https://downloads.sourceforge.net/project/lame/lame/$LAME_VERSION/lame-$LAME_VERSION.tar.gz" "lame-$LAME_VERSION"
    cyan_do_confmakeinstall --disable-{frontend,decoder,gtktest}
}

build_libogg() {
    cyan_do_tarball "https://downloads.xiph.org/releases/ogg/libogg-$LIBOGG_VERSION.tar.gz" "libogg-$LIBOGG_VERSION"
    cyan_do_confmakeinstall
}

build_libvorbis() {
    cyan_do_tarball "https://downloads.xiph.org/releases/vorbis/libvorbis-$LIBVORBIS_VERSION.tar.xz" "libvorbis-$LIBVORBIS_VERSION"
    # The configure script hardcodes -force_cpusubtype_ALL on macOS, which
    # the current Xcode linker rejects; the CMake build doesn't.
    cyan_do_cmakeinstall
}

build_opus() {
    cyan_do_tarball "https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz" "opus-$OPUS_VERSION"
    cyan_do_confmakeinstall --disable-{doc,extra-programs}
}

build_ffmpeg() {
    cyan_do_tarball "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" "ffmpeg-$FFMPEG_VERSION"
    # Same feature set as the MinGW build (.github/mingw-build.sh), plus the
    # pcm_f64le encoder which the raw PCM output requires.
    ./configure --prefix="$CYANRIPINSTALLDIR" --pkg-config-flags=--static \
        --disable-shared --enable-static \
        --disable-{programs,devices,filters,decoders,hwaccels,encoders,muxers} \
        --disable-{debug,protocols,demuxers,parsers,doc,swscale,network} \
        --disable-{avdevice,autodetect} \
        --disable-bsfs --enable-protocol=file,data \
        --enable-encoder=flac,tta,aac,wavpack,alac,pcm_s16le,pcm_s32le,pcm_f64le \
        --enable-muxer=flac,tta,ipod,wv,mp3,opus,ogg,wav,pcm_s16le,pcm_s32le,image2,singlejpeg \
        --enable-parser=png,mjpeg --enable-decoder=mjpeg,png \
        --enable-demuxer={image2,image_jpeg_pipe,image_png_pipe} \
        --enable-{bzlib,zlib,iconv} \
        --enable-filter={hdcd,aemphasis,ebur128,anullsink,aresample} \
        --enable-lib{mp3lame,vorbis,opus} \
        --enable-encoder={libmp3lame,libvorbis,libopus}
    make -j"$CYANRIPJOBS"
    make install
}

build_cyanrip() {
    local link_args=()
    # A fully static binary is only possible on Linux (with musl); on macOS
    # the system libraries and frameworks stay dynamic.
    if [[ $CYANRIPOS == linux ]]; then
        link_args+=(-Dc_link_args=-static)
    fi

    local builddir="$CYANRIPBUILDDIR/cyanrip"
    cd "$CYANRIPREPODIR"
    rm -rf "$builddir"
    meson setup "$builddir" --buildtype=release --default-library=static \
        --prefer-static "${link_args[@]}"
    ninja -C "$builddir"
    meson test -C "$builddir" --print-errorlogs
    strip -o cyanrip "$builddir/src/cyanrip"
}

# With no arguments everything is built; otherwise only the named steps
# (e.g. build_ffmpeg build_cyanrip), for iterating on one of them.
cyan_prepare
if [[ $# -eq 0 ]]; then
    buildCyanrip
else
    for step; do "$step"; done
fi
