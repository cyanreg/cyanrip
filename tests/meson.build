## Unit tests
## ==========
## Built from the objects they test, avtransport-style.
unit_test_deps = [
    dependency('libavutil'),
    dependency('libavcodec'),
    dependency('libcdio'),
    dependency('libcdio_paranoia'),
]

fun512_test = executable('fun512_test',
    sources: [ 'fun512.c' ],
    include_directories: [ '../src' ],
    objects: cyanrip_exe.extract_objects([ 'fun512.c' ]),
    dependencies: unit_test_deps,
)
test('FUN512 checksum', fun512_test)

naming_test = executable('naming_test',
    sources: [ 'naming.c' ],
    include_directories: [ '../src' ],
    objects: cyanrip_exe.extract_objects([ 'naming.c' ]),
    dependencies: unit_test_deps,
)
test('Naming schemes', naming_test)

pregap_test = executable('pregap_test',
    sources: [ 'pregap_test.c' ],
    include_directories: [ '../src' ],
    objects: cyanrip_exe.extract_objects([ 'pregap.c' ]),
    dependencies: unit_test_deps,
)
test('Pregap search', pregap_test)

## Integration tests
## =================
## Rip the disc image fixtures with the built binary and verify the
## finished files.
python = import('python').find_installation()

rip_scenarios = [
    'info',
    'basic',
    'pregap',
    'mixed',
    'nrg',
    'filters',
    'art',
    'cue_only',
    'errors',
    'verify_log',
]

foreach s : rip_scenarios
    test(s, python,
         args: [ files('rip_images.py'), cyanrip_exe,
                 meson.current_source_dir() / 'fixtures', s ],
         suite: 'images',
         timeout: 120)
endforeach
