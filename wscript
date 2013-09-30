#! /usr/bin/env python
# encoding: utf-8

from waflib import Utils

top = '.'
out = '__build__'

APPNAME = "squint"
VERSION = "0.5"

def options (ctx):
	ctx.load ("compiler_c")

def configure (conf):
	conf.load ("compiler_c")

	conf.define("PREFIX", conf.env.PREFIX)
	if not	conf.check_cfg(package="gtk+-3.0", args="--cflags --libs", uselib_store="GTK", mandatory = False):
		conf.check_cfg(package="gtk+-2.0", args="--cflags --libs", uselib_store="GTK")

	conf.find_program("txt2tags", mandatory=False)
	conf.find_program("gzip", mandatory=False)

def build (bld):

	bld.program(
		target="squint",
		source="squint.c",
		uselib="GTK"
	)

	bld.install_as("${PREFIX}/share/squint/squint.png", "squint.png")
	
	if bld.env.TXT2TAGS and bld.env.GZIP:
		bld(
			target="squint.1.gz",
			source="squint.1.t2t",
			rule="${TXT2TAGS} -o - -t man '${SRC}' | ${GZIP} > '${TGT}'",
		)

		bld.install_as("${PREFIX}/share/man/man1/squint.1.gz", "squint.1.gz")

