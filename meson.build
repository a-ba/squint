project('squint', 'c')

cfg = configuration_data()
cfg.set_quoted('APPNAME', 'squint')
cfg.set_quoted('VERSION', '0.7.1')
cfg.set_quoted('PREFIX', get_option('prefix'))

deps = [
	dependency('gtk+-3.0'),
	dependency('x11'),
]

have_all_deps = true
foreach d: [
	['ayatana-appindicator3-0.1',	'HAVE_APPINDICATOR'],
	['xdamage',			'HAVE_XDAMAGE'],
	['xfixes',			'HAVE_XFIXES'],
	['xi',				'HAVE_XI'],
	['xrandr',			'HAVE_XRANDR'],
	['xext',			'HAVE_XEXT'],
]
	dep = dependency(d[0], required: false)
	if dep.found()
		deps += dep
		cfg.set(d[1], 1)
	else
		have_all_deps = false
	endif
endforeach

if cfg.has('HAVE_XFIXES') and cfg.has('HAVE_XDAMAGE')
	cfg.set('USE_XDAMAGE', 1)
endif

if cfg.has('HAVE_XFIXES') and cfg.has('HAVE_XEXT')
	cfg.set('COPY_CURSOR', 1)
endif

if not have_all_deps
	warning('NOTE: one or more libraries were not found on your system, squint will work in degraded mode')
endif

configure_file(configuration: cfg, output: 'config.h')

executable('squint', 'squint.c', 'x11.c', dependencies: deps, install: true)
install_data('squint.png')
install_data('squint-disabled.png')

t2t = find_program('txt2tags')
gzip = find_program('gzip')

if t2t.found() and gzip.found()
	man = custom_target('squint.1',
		input: 'squint.1.t2t',
		output: 'squint.1',
		command: [t2t, '-t', 'man', '-o', '@OUTPUT@', '@INPUT@'])
	custom_target('squint.1.gz',
		build_by_default: true,
		input: man,
		output: 'squint.1.gz',
		install: true,
		install_dir: get_option('mandir') + '/man1',
		capture: true,
		command: [gzip, '-c9', '@INPUT@'])

	custom_target('squint.1.html',
		build_by_default: true,
		input: 'squint.1.t2t',
		output: 'squint.1.html',
		command: [t2t, '-t', 'html', '@INPUT@'])
endif

