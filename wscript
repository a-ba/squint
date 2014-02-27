#! /usr/bin/env python
# encoding: utf-8

from waflib import Logs

top = '.'
out = '__build__'

APPNAME = "squint"
VERSION = "0.6"

def options (ctx):
	ctx.load ("compiler_c")

def configure (conf):
	conf.load ("compiler_c")
	
	conf.define("APPNAME", APPNAME);
	conf.define("VERSION", VERSION);

	conf.define("PREFIX", conf.env.PREFIX)

	conf.check_cfg(package="gtk+-3.0", args="--cflags --libs", uselib_store="GTK")
	
	conf.check_cfg(package="xi", args="--cflags --libs", uselib_store="XI",
			atleast_version="1.5", mandatory=False)

	conf.check_cfg(package="xrandr", args="--cflags --libs", uselib_store="XRANDR",
			mandatory=False)

	conf.check_cfg(package="libnotify", args="--cflags --libs", uselib_store="LIBNOTIFY",
			mandatory=False)

	have_xfixes = conf.check_cfg(package="xfixes", args="--cflags --libs",
			uselib_store="XFIXES", mandatory=False)

	if (have_xfixes and
		conf.check_cfg(package="xdamage", args="--cflags --libs", uselib_store="XDAMAGE",
			mandatory=False)
	):
		conf.define("USE_XDAMAGE", 1)

	if (have_xfixes and
		conf.check_cfg(package="xext", args="--cflags --libs",
			uselib_store="XEXT", mandatory=False)
	):
		conf.define("COPY_CURSOR", 1)

	conf.find_program("txt2tags", mandatory=False)
	conf.find_program("gzip", mandatory=False)

	if any(((x not in conf.env.define_key) for x in 
		('HAVE_XI', 'HAVE_XRANDR', 'HAVE_LIBNOTIFY',
		 'HAVE_XFIXES', 'HAVE_XDAMAGE', 'HAVE_XEXT',
		))):
			Logs.pprint("YELLOW", "NOTE: one or more libraries were not found on your system, squint will work in degraded mode");

def build (bld):

	bld.program(
		target="squint",
		source="squint.c",
		uselib="GTK XI XFIXES XEXT XDAMAGE XRANDR LIBNOTIFY"
	)

	bld.install_as("${PREFIX}/share/squint/squint.png", "squint.png")
	bld.install_as("${PREFIX}/share/squint/squint-disabled.png", "squint-disabled.png")
	
	if bld.env.TXT2TAGS and bld.env.GZIP:
		bld(
			target="squint.1.gz",
			source="squint.1.t2t",
			rule="${TXT2TAGS} -o - -t man '${SRC}' | ${GZIP} > '${TGT}'",
		)
		bld(
			target="squint.1.html",
			source="squint.1.t2t",
			rule="${TXT2TAGS} -o - -t html '${SRC}' > '${TGT}'",
		)

		bld.install_as("${PREFIX}/share/man/man1/squint.1.gz", "squint.1.gz")

