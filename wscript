#! /usr/bin/env python
# encoding: utf-8

from waflib import Utils

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
	if not	conf.check_cfg(package="gtk+-3.0", args="--cflags --libs", uselib_store="GTK", mandatory = False):
		conf.check_cfg(package="gtk+-2.0", args="--cflags --libs", uselib_store="GTK")
	
	conf.check_cfg(package="xi", args="--cflags --libs", uselib_store="XI",
			atleast_version="1.5", mandatory=False)

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

def build (bld):

	bld.program(
		target="squint",
		source="squint.c",
		uselib="GTK XI XFIXES XEXT XDAMAGE"
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

