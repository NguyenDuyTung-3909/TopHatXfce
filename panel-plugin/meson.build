plugin_sources = [
    'collector.c',
    'collector_cpu.c',
    'collector_mem.c',
    'data.c',
    'defaults.c',
    'drawing.c',
    'tophatxfce.c',
    'info_cpu.c',
    'info_mem.c',
    'panel_entry.c',
    'plugin.c',
    'settings.c',
    'popup.c',
    'sysinfo.c',
    xfce_revision_h,
]

plugin_install_subdir = 'xfce4' / 'panel' / 'plugins'

plugin_lib = shared_module(
  'tophatxfce',
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
