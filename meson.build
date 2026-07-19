project('atelier-arland-menu-fix', 'cpp', version : '1.0.0',
  default_options : ['cpp_std=c++17', 'buildtype=release'])

cpp = meson.get_compiler('cpp')
shared_library('d3d11', 'main.cpp',
  name_prefix : '',
  vs_module_defs : 'd3d11.def',
  cpp_args : ['-DNOMINMAX'],
  link_args : cpp.get_id() == 'gcc'
    ? ['-static', '-static-libgcc', '-static-libstdc++'] : [],
  install : true)
