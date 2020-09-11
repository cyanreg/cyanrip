#!/bin/bash

buildCyanrip() {
    cyan_prepare
    build_curl
    build_neon
    build_libmusicbrainz
    # build_libcoverart
    build_ffmpeg
    build_cyanrip
}

cyan_prepare() {
    local rootdir="$(pwd)"
    CYANRIPREPODIR="${GITHUB_WORKSPACE:-$(realpath "$rootdir/..")}"
    CYANRIPINSTALLDIR="$(realpath "$rootdir/_install")"
    CYANRIPBUILDDIR="$(realpath "$rootdir/_build")"
    PKG_CONFIG_PATH=$CYANRIPINSTALLDIR/lib/pkgconfig:$PKG_CONFIG_PATH
    CPATH="$(cygpath -pm $CYANRIPINSTALLDIR/include:$MINGW_PREFIX/include)"
    LIBRARY_PATH="$(cygpath -pm $CYANRIPINSTALLDIR/lib:$MINGW_PREFIX/lib)"
    CPPFLAGS="-D_FORTIFY_SOURCE=0 -D__USE_MINGW_ANSI_STDIO=1"
    CFLAGS="-mthreads -mtune=generic -O2 -pipe"
    CXXFLAGS="${CFLAGS}"
    LDFLAGS="-pipe -static-libgcc -static-libstdc++"
    export CYANRIPREPODIR CYANRIPBUILDDIR CYANRIPINSTALLDIR PKG_CONFIG_PATH CPATH LIBRARY_PATH CPPFLAGS CFLAGS CXXFLAGS LDFLAGS
    mkdir -p "$CYANRIPBUILDDIR" "$CYANRIPINSTALLDIR"

    cyan_do_hide_all_sharedlibs
}

cyan_do_vcs() {
    local vcsURL=${1#*::} vcsFolder=$2
    local vcsBranch=${vcsURL#*#} ref=origin/HEAD
    : "${vcsFolder:=$(basename "$vcsURL" .git)}"

    cd "$CYANRIPBUILDDIR"
    git clone --depth 1 "$vcsURL" "$vcsFolder"
    cd "$vcsFolder"
}

cyan_do_hide_all_sharedlibs() {
    local files
    files="$(find /mingw{32,64}/lib /mingw{32/i686,64/x86_64}-w64-mingw32/lib -name "*.dll.a" 2> /dev/null)"
    local tomove=()
    for file in $files; do
        [[ -f ${file%*.dll.a}.a ]] && tomove+=("$file")
    done
    printf '%s\0' "${tomove[@]}" | xargs -0ri mv -f '{}' '{}.dyn'
}

cyan_hide_files() {
    for opt; do [[ -f $opt ]] && mv -f "$opt" "$opt.bak"; done
}
cyan_hide_conflicting_libs() {
    local -a installed
    mapfile -t installed < <(find "$CYANRIPINSTALLDIR/lib" -maxdepth 1 -name "*.a")
    cyan_hide_files "${installed[@]//$CYANRIPINSTALLDIR/$MINGW_PREFIX}"
}

cyan_do_separate_confmakeinstall() {
    rm -rf ./_build &&
    mkdir _build && cd _build &&
    ../configure --disable-shared --enable-static --prefix="$CYANRIPINSTALLDIR" "$@" &&
    make && make install
}

cyan_do_cmakeinstall() {
    PKG_CONFIG=pkg-config cmake -B _build -G Ninja -DBUILD_SHARED_LIBS=off \
            -DCMAKE_INSTALL_PREFIX="$CYANRIPINSTALLDIR" -DUNIX=on \
            -DCMAKE_FIND_ROOT_PATH="$(cygpath -pm "$CYANRIPINSTALLDIR:$MINGW_PREFIX:$MINGW_PREFIX/$MINGW_CHOST")" \
            -DCMAKE_PREFIX_PATH="$(cygpath -pm "$CYANRIPINSTALLDIR:$MINGW_PREFIX:$MINGW_PREFIX/$MINGW_CHOST")" \
            -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
            -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
            -DCMAKE_BUILD_TYPE=Release "$@" &&
        ninja -C _build && ninja -C _build install
}

build_curl() {
    cyan_do_vcs "https://github.com/curl/curl.git" &&
    autoreconf -fi &&
    sed -ri "s;(^SUBDIRS = lib) src (include) scripts;\1 \2;" Makefile.in &&
    CPPFLAGS+=" -DNGHTTP2_STATICLIB" \
    cyan_do_separate_confmakeinstall --with-{winssl,winidn,nghttp2} \
        --without-{ssl,gnutls,mbedtls,libssh2,random,ca-bundle,ca-path,librtmp,brotli,debug,libpsl}
}

build_neon() {
    cyan_do_vcs "https://github.com/notroj/neon.git" &&
    ./autogen.sh &&
    cyan_do_separate_confmakeinstall --disable-{nls,debug,webdav}
}

build_libmusicbrainz() {
    cyan_do_vcs "https://github.com/wiiaboo/libmusicbrainz.git" &&
    cyan_do_cmakeinstall
}

build_libcoverart() {
    cyan_do_vcs "https://github.com/wiiaboo/libcoverart.git" &&
    cyan_do_cmakeinstall
}

build_ffmpeg() {
    cyan_do_vcs "https://git.ffmpeg.org/ffmpeg.git"
    cyan_do_separate_confmakeinstall --pkg-config-flags=--static \
        --disable-{programs,devices,filters,decoders,hwaccels,encoders,muxers} \
        --disable-{debug,protocols,demuxers,parsers,doc,swscale,postproc,network} \
        --disable-{avdevice,autodetect} \
        --disable-bsfs --enable-protocol=file \
        --enable-encoder=flac,tta,aac,wavpack,alac,pcm_s16le,pcm_s32le \
        --enable-muxer=flac,tta,ipod,wv,mp3,opus,ogg,wav,pcm_s16le,pcm_s32le \
        --enable-parser=png,mjpeg --enable-decoder=mjpeg,png \
        --enable-demuxer=image2,png_pipe,bmp_pipe \
        --enable-{bzlib,zlib,lzma,iconv} \
        --enable-filter=hdcd \
        --enable-lib{mp3lame,vorbis,opus} \
        --enable-encoder={libmp3lame,libvorbis,libopus}
}

build_cyanrip() {
    cd "$CYANRIPREPODIR"
    cyan_hide_conflicting_libs
    PKG_CONFIG=pkg-config \
    CFLAGS+=" -DLIBXML_STATIC -DCURL_STATICLIB $(printf ' -I%s' "$(cygpath -m "$CYANRIPINSTALLDIR/include")")" \
        LDFLAGS+="$(printf ' -L%s' "$(cygpath -m "$CYANRIPINSTALLDIR/lib")")" \
        meson build --default-library=static --buildtype=release --prefix="$CYANRIPINSTALLDIR" --backend=ninja &&
    ninja -C build && strip --strip-all build/src/cyanrip.exe -o cyanrip.exe
}

cd "$(dirname "$0")"
buildCyanrip
