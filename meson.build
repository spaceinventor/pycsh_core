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
	vcs_tag(input: files('src/version.c.in'), output: 'version.c', command: ['git', 'describe', '--long', '--always', '--dirty=+'])
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

apm_csh_dep = dependency('apm_csh', fallback: ['apm_csh', 'apm_csh_dep'], required: true).partial_dependency(
	links: false, 
	includes: true
)
param_dep = dependency('param', fallback: ['param', 'param_dep'], required: true)
csp_dep = dependency('csp', fallback: ['csp', 'csp_dep'], required: true)


dependencies += [
	apm_csh_dep,
	param_dep,
	csp_dep,
	slash_dep,
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

pycsh_deps = [
	python3_dep,
	apm_csh_dep,
	param_dep,
	csp_dep,
	# the .as_link_whole() is to force linking of the builtin commands
	slash_dep.as_link_whole(),
]
py = import('python').find_installation('python'+get_option('python3_version'), pure: false)
python_ldflags = run_command('python'+py.language_version()+'-config', '--ldflags', '--embed', check: true).stdout().strip().split()
pycsh_ext = py.extension_module(
	'pycsh',
	pycsh_sources,
	dependencies : pycsh_deps,
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
