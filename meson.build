project('cyanrip', 'c',
    license: 'LGPL2.1+',
    default_options: [ 'buildtype=debugoptimized', 'c_std=c99'],
)

conf = configuration_data()

majorver = '0'
minorver = '1'
microver = '0'

version = majorver + '.' + minorver + '.' + microver

conf.set_quoted('CYANRIP_VERSION_STRING', version)

configure_file(
    output: 'config.h',
    configuration: conf,
)

subdir('src')
