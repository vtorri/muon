# SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
# SPDX-License-Identifier: GPL-3.0-only

scripts_input = []
scripts_cmdline = []

foreach s : [
    'dependencies.meson',
    'copyfile.meson',
    'global_options.meson',
    'per_project_options.meson',
    'vcs_tagger.meson',
    'gettext.meson',
    'itstool.meson',
    'msgfmthelper.meson',
    'python_info.py',

    'modules/_test.meson',
    'modules/i18n.meson',
    'modules/gnome.meson',

    'html/test_out.html',
]
    scripts_input += s
    scripts_cmdline += files(s) + [s]
endforeach

script_sources = custom_target(
    'embedded files',
    input: scripts_input,
    output: 'embedded_files.h',
    capture: true,
    command: [embedder, scripts_cmdline],
)
