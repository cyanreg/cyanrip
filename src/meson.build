build_opts = [
    '-D_ISOC99_SOURCE', '-D_GNU_SOURCE', '-D_XOPEN_SOURCE=700',
    '-U__STRICT_ANSI__', '-fvisibility=hidden',

    # Warnings
    '-Wall', '-Wundef', '-Wmissing-prototypes', '-Wshadow', '-Wparentheses',
    '-Wpointer-arith', '-Wno-pointer-sign',

    # Warnings to treat as errors
    '-Werror=implicit-function-declaration',
]

cc = meson.get_compiler('c')

static_build = get_option('default_library') == 'static'

dependencies = [
    dependency('libcdio', version: '>= 2.0'),
    dependency('libcdio_paranoia', version: '>= 10.2'),
    dependency('libdiscid', version: '>= 0.6'),
    dependency('libmusicbrainz5', version: '>= 5.1', static: static_build),
    dependency('libavcodec', version: '>= 58.19.100'),
    dependency('libavformat', version: '>= 58.13.100'),
    dependency('libswresample', version: '>= 3.2.100'),
    dependency('libavutil', version: '>= 56.15.100'),
    dependency('threads'),
]

# Check for wmain support (Windows/MinGW)
if cc.links('int wmain(){return 0;}', args: '-municode')
    conf.set('HAVE_WMAIN', 1)
    build_opts += '-municode'
endif

components = [
    'cyanrip_encode.c',
    'cyanrip_log.c',
    'cyanrip_main.c',
]

executable('cyanrip', install: true, sources: components, dependencies: dependencies)
