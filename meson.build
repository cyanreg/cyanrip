project('cyanrip', 'c',
    license: 'LGPL2.1+',
    default_options: [ 'buildtype=debugoptimized', 'c_std=c99'],
    version: '0.4.3',
    meson_version: '>=0.41.0',
)

conf = configuration_data()

conf.set_quoted('CYANRIP_VERSION_STRING', meson.project_version())

subdir('src')

configure_file(
    output: 'config.h',
    configuration: conf,
)
