project('pycsh_core', 'c')

conf = configuration_data()
include_dir = include_directories('.', 'include')
python3_dep = dependency('python3', required: true)  # We don't actually need Python as dep here when we have the linker args, but it might help VSC with highlighting.
dependencies = [
	python3_dep
]
slash_dep = dependency('slash', fallback: ['slash', 'slash_dep'], required: true)

pycsh_sources = [

	# Module
	'src/pycsh.c',

	# Classes
	'src/parameter/parameter.c',
	'src/parameter/pythongetsetparameter.c',
	'src/parameter/parameterlist.c',
	'src/parameter/valueproxy.c',
	'src/csp_classes/ident.c',
	'src/csp_classes/vmem.c',
	'src/csp_classes/info.c',
	'src/csp_classes/route.c',
	'src/csp_classes/iface.c',
	'src/csp_classes/ifstat.c',
	#'src/csp_classes/node.c',  # Coming soon...

	# Wrapper functions
	'src/wrapper/py_csp.c',
	'src/wrapper/param_py.c',
	'src/wrapper/dflopt_py.c',
	'src/wrapper/csp_init_py.c',
	'src/wrapper/spaceboot_py.c',
	'src/wrapper/param_list_py.c',
	'src/wrapper/vmem_client_py.c',
	# 'src/wrapper/victoria_metrics_py.c',

	# Utilities
	'src/utils.c',
	vcs_tag(input: files('src/version.c.in'), output: 'version.c', command: ['git', 'describe', '--long', '--always', '--dirty=+']),
]

if slash_dep.found()
	pycsh_sources += [
		'src/wrapper/slash_py.c',
		'src/slash_command/slash_command.c',
		'src/slash_command/python_slash_command.c',
	]
endif
conf.set('PYCSH_HAVE_SLASH', slash_dep.found())

pycsh_config_h = configure_file(output: 'pycshconfig.h', configuration: conf, install_dir: 'include/csh/', install: false)

apm_csh_dep = dependency('apm_csh', fallback: ['apm_csh', 'apm_csh_dep'], required: true)
param_dep = dependency('param', fallback: ['param', 'param_dep'], required: true)
csp_dep = dependency('csp', fallback: ['csp', 'csp_dep'], required: true)

# Dependencies meant for libraries must use the .partial_dependency() construct below
# so that meson does not include the object files of the dependencies in the final library.
# This is important for libraries that are meant to be linked into other libraries or executables, as it avoids "duplicate symbols" errors
# when linking final executables/shared libraries.
dependencies += [
	apm_csh_dep.partial_dependency(
		links: false,
		includes: true
	),
	param_dep.partial_dependency(
		links: false,
		includes: true
	),
	csp_dep.partial_dependency(
		links: false,
		includes: true
	),
	slash_dep.partial_dependency(
		links: false,
		includes: true
	)
]

pycsh_lib = library(
	'pycsh',
	sources: pycsh_sources,
	include_directories: [include_dir],
	dependencies: dependencies
)
pycsh_core_dep = declare_dependency(
	link_with: pycsh_lib,
	include_directories: [include_dir],
	dependencies: dependencies
)

# The .as_link_whole() constructs below force inclusion of all public symbols into the pycsh shared library,
# not just the symbols used directly by pycsh.
# This is important so that other Python bindings can depend and use those symbols
pycsh_deps = [
	python3_dep,
	apm_csh_dep.as_link_whole(),
	param_dep.as_link_whole(),
	csp_dep.as_link_whole(),
	slash_dep.as_link_whole(),
]
py = import('python').find_installation('python'+get_option('python3_version'), pure: false)
python_ldflags = run_command('python'+py.language_version()+'-config', '--ldflags', '--embed', check: true).stdout().strip().split()
pycsh_ext = py.extension_module(
	'pycsh',
	pycsh_sources + [
		# CSH sources
		# TODO: refactor these into a separate library, to be used by both pycsh and csh
		'src/csh/python_host.c',
		'src/csh/known_hosts.c',
		'src/csh/csh_defaults.c',
		'src/csh/environment.c',

		'src/csh/param_slash.c',
		'src/csh/slash_utils.c',
		'src/csh/param_list_slash.c',
	],
	dependencies : pycsh_deps + [
		dependency('libcurl', not_found_message: 'libcurl not found! Please install libcurl4-openssl-dev or the appropriate package for your system.'),
	],
	include_directories: [include_dir],
	link_args : python_ldflags + ['-Wl,-Map=' + meson.project_name() + '.map'],
	install : get_option('install'),
	subdir: 'pycsh',
	gnu_symbol_visibility: 'default',
)

if get_option('install')
	# Add .pyi file for type-hints
	pyi = configure_file(input: 'pycsh.pyi', output: 'pycsh.pyi', copy: true)
	__init__py = configure_file(input: '__init__.py', output: '__init__.py', copy: true)
	py.install_sources([pyi, __init__py], subdir: 'pycsh')
endif
