from __future__ import print_function
import sys, os, re
from waflib.Build import BuildContext

APPNAME='cyanrip'
VERSION='0.1-rc2'

top = '.'
out = 'build'

def try_git_version():
    version = VERSION
    try:
        version = os.popen('git rev-parse --short=10 HEAD').read().strip()
    except Exception as e:
        print(e)
    return version

def try_pkg_path(name):
    path = ''
    cmd = 'pkg-config --cflags-only-I ' + name
    try:
        path = os.popen(cmd).read().strip()
        path = os.path.basename(path)
    except Exception as e:
        print(e)
    return path

def options(ctx):
    ctx.load('compiler_c')
    ctx.add_option('--no-debug', dest='no_deb', action='store_true', default=False,
                help='Unsets debug compilation flags.')
    ctx.add_option('--static-build', dest='static_build', action='store_true',
                default=False, help='Do static build instead of shared.')

def configure(ctx):
    ctx.load('compiler_c')

    if (ctx.options.no_deb == False):
        ctx.env.CFLAGS += ['-g']

    pkgcnf_args = ['--cflags', '--libs']
    if (ctx.options.static_build):
        pkgcnf_args += ['--static']

    ctx.env.append_unique('CFLAGS', ['-O2', '-Wall', '-pedantic', '-std=gnu11'])

    ctx.check_cfg(package='libcdio', args=pkgcnf_args, uselib_store='CDIO')
    ctx.check_cfg(package='libcdio_paranoia', args=pkgcnf_args, uselib_store='PARA')
    ctx.check_cfg(package='libavcodec', args=pkgcnf_args, uselib_store='LAVC')
    ctx.check_cfg(package='libavformat', args=pkgcnf_args, uselib_store='LAVF')
    ctx.check_cfg(package='libavutil', args=pkgcnf_args, uselib_store='LAVU')
    ctx.check_cfg(package='libswresample', args=pkgcnf_args, uselib_store='LSWR')
    ctx.check_cfg(package='libdiscid', args=pkgcnf_args, uselib_store='DISCID')
    ctx.check_cfg(package='libmusicbrainz5', args=pkgcnf_args, uselib_store='MB')
    ctx.check_cc(msg='Checking for the new paranoia API', header_name='cdio/paranoia/paranoia.h', mandatory=False)

    if ctx.env.DEST_OS == 'win32':
        ctx.check_cc(msg='Checking for wmain() support', fragment='int wmain() {return 0;}\n',
            cflags="-municode", linkflags="-municode", uselib_store='WMAIN', define_name='HAVE_WMAIN')

    if (VERSION):
        package_ver = VERSION
    else:
        package_ver = try_git_version() + '-git'

    FULL_PACKAGE_NAME = APPNAME + ' ' + package_ver

    ctx.define('PROGRAM_NAME', APPNAME)
    ctx.define('PROGRAM_VERSION', package_ver)
    ctx.define('PROGRAM_STRING', FULL_PACKAGE_NAME)

    ctx.write_config_header('config.h')

    print('	CFLAGS:  	', ctx.env.CFLAGS)

def build(ctx):
    ctx(name='cyanrip_encode',
        path=ctx.path,
        uselib=[ 'LAVC', 'LAVF', 'LSWR', 'LAVU' ],
        target='cyanrip_encode',
        source='src/cyanrip_encode.c',
        features  = ['c'],
        includes='. .. ../../',
    )
    ctx(name='cyanrip_log',
        path=ctx.path,
        uselib=[ ],
        target='cyanrip_log',
        source='src/cyanrip_log.c',
        features  = ['c'],
        includes='. .. ../../',
    )
    ctx(name='cyanrip',
        path=ctx.path,
        uselib=['CDIO', 'PARA', 'MB', 'LAVC', 'LAVF', 'LSWR', 'LAVU', 'DISCID', 'WMAIN' ],
        use=['in_file', 'cyanrip_encode', 'cyanrip_log'],
        target=APPNAME,
        source='src/cyanrip_main.c',
        features  = ['c', 'cprogram'],
        includes='. .. ../../',
    )
