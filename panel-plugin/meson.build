plugin_sources = [
    'collector_cpu.c',
    'collector_cpu.h',
    'collector_mem.c',
    'collector_mem.h',
    'collector.c',
    'collector.h',
    'data.c',
    'data.h',
    'defaults.c',
    'defaults.h',
    'drawing.c',
    'drawing.h',
    'info_cpu.c',
    'info_cpu.h',
    'info_mem.c',
    'info_mem.h',
    'panel_entry.c',
    'panel_entry.h',
    'plugin.c',
    'plugin.h',
    'settings.c',
    'settings.h',
    'tophatxfce.c',
    'tophatxfce.h',
    'utils.h',
    xfce_revision_h,
]

plugin_install_subdir = 'xfce4' / 'panel' / 'plugins'

plugin_lib = shared_module(
  'top_xfce',
    plugin_sources,
    gnu_symbol_visibility: 'hidden',
    c_args: [
        '-DG_LOG_DOMAIN="@0@"'.format('tophatxfce-plugin'),
    ],
    include_directories: [
        include_directories('..'),
    ],
    dependencies: [
        glib,
        gtk,
        libxfce4panel,
        libxfce4ui,
        libxfce4util,
    ],
    install: true,
    install_dir: get_option('prefix') / get_option('libdir') / plugin_install_subdir,
)

i18n.merge_file(
    input: 'tophatxfce.desktop.in',
    output: 'tophatxfce.desktop',
    po_dir: '..' / 'po',
    type: 'desktop',
    install: true,
    install_dir: get_option('prefix') / get_option('datadir') / plugin_install_subdir,
)
