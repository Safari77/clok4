# meson.build

project(
  'clok4',
  'c',
  version : '0.4.2',
  license : 'GPL-2.0-or-later',
  meson_version: '>= 1.0.0',
  default_options : [
    'warning_level=3',
    'c_std=c11'  # or 'gnu11'/'c99', as you prefer
  ]
)

cc = meson.get_compiler('c')

# Detect dependencies
gtk_dep   = dependency('gtk4', version: '>=4.0')
rsvg_dep  = dependency('librsvg-2.0')
glib_dep  = dependency('glib-2.0')     # used in the original code
math_lib = cc.find_library('m', required: true)

# Source files for the main executable
srcs = [
  'clok4.c',
]

executable_name = 'clok4'

exe = executable(
  executable_name,
  srcs,
  dependencies : [
    gtk_dep,
    rsvg_dep,
    glib_dep,
    math_lib
  ],
  include_directories : include_directories('.'),
  install : true  # installs into bin/ by default
)

