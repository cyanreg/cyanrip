project('cyanrip', 'c',
    license: 'LGPL2.1+',
    default_options: [ 'buildtype=debugoptimized', 'c_std=c99', 'warning_level=1' ],
    version: '0.9.0',
    meson_version: '>=0.53.0',
)

conf = configuration_data()

conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION_STRING', meson.project_version())

version_split = meson.project_version().split('.')
ver_major = version_split[0]
ver_minor = version_split[1]
ver_micro = version_split[2]

conf.set('PROJECT_VERSION_MAJOR', ver_major)
conf.set('PROJECT_VERSION_MINOR', ver_minor)
conf.set('PROJECT_VERSION_MICRO', ver_micro)
if host_machine.endian() == 'big'
    conf.set('CONFIG_BIG_ENDIAN', 1)
else
    conf.set('CONFIG_BIG_ENDIAN', 0)
endif

cc = meson.get_compiler('c')

subdir('src')

configure_file(
    output: 'config.h',
    configuration: conf,
)
