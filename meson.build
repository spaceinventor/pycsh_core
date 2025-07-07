project('pycsh_core', 'c')

conf = configuration_data()
include_dir = include_directories('.', 'include')
dependencies = [
	dependency('python3', required: true),  # We don't actually need Python as dep here when we have the linker args, but it might help VSC with highlighting.
]
slash_dep = dependency('slash', fallback: ['slash', 'slash_dep'], required: true)

pycsh_sources = [

	# Module
	'src/pycsh.c',

	# Classes
	'src/parameter/parameter.c',
	'src/parameter/parameterarray.c',
	'src/parameter/pythonparameter.c',
	'src/parameter/pythonarrayparameter.c',
	'src/parameter/pythongetsetparameter.c',
	'src/parameter/pythongetsetarrayparameter.c',
	'src/parameter/parameterlist.c',
	'src/csp_classes/ident.c',
	'src/csp_classes/vmem.c',
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

dependencies += [
	dependency('apm_csh', fallback: ['apm_csh', 'apm_csh_dep'], required: true).partial_dependency(
		links: false, 
		includes: true
	),
	dependency('param', fallback: ['param', 'param_dep'], required: true),
	dependency('csp', fallback: ['csp', 'csp_dep'], required: true),
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
