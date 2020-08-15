build_opts = [
    '-D_ISOC11_SOURCE', '-D_XOPEN_SOURCE=700', '-U__STRICT_ANSI__',

    # Warnings
    '-Wundef', '-Wmissing-prototypes', '-Wshadow', '-Wparentheses',
    '-Wpointer-arith', '-Wno-pointer-sign',

    # Warnings to treat as errors
    '-Werror=implicit-function-declaration',
]

static_build = get_option('default_library') == 'static'

# Required dependencies
dependencies = [
    # ffmpeg libs
    dependency('libavcodec', version: '>= 58.82.100'),
    dependency('libavformat', version: '>= 58.42.100'),
    dependency('libswresample', version: '>= 3.6.100'),
    dependency('libavfilter', version: '>= 7.79.100'),
    dependency('libavutil', version: '>= 56.43.100'),

    # other dependencies
    dependency('libcdio', version: '>= 2.0'),
    dependency('libcdio_paranoia', version: '>= 10.2'),
    dependency('libdiscid', version: '>= 0.6'),
    dependency('libmusicbrainz5', version: '>= 5.1', static: static_build),

    # misc
    dependency('threads'),
    cc.find_library('m', required : true),
]

# Base files
sources = [
    'cyanrip_encode.c',
    'cyanrip_log.c',
    'cyanrip_main.c',

    'fifo_frame.c',

    # Version
    vcs_tag(command: ['git', 'rev-parse', '--short', 'HEAD'],
                      input: 'version.c.in', output: 'version.c',
                      fallback: 'release')
]

# Check for wmain support (Windows/MinGW)
if cc.links('int wmain() { return 0; }', args: '-municode')
     conf.set('HAVE_WMAIN', 1)
     build_opts += '-municode'
endif

add_global_arguments(build_opts, language: 'c')

executable('cyanrip',
    install: true,
    sources: sources,
    dependencies: dependencies,
)
