project('atelier-arland-fixes', ['c', 'cpp'], version : '0.1.0',
  default_options : ['cpp_std=c++17', 'buildtype=release', 'strip=true'])

cpp = meson.get_compiler('cpp')
sources = files(
  'src/main.cpp',
  'src/menu_fix.cpp',
  'src/sync_fix.cpp',
  'vendor/minhook/src/hde/hde64.c',
  'vendor/minhook/src/hook.c',
  'vendor/minhook/src/buffer.c',
  'vendor/minhook/src/trampoline.c',
)

shared_library('d3d11', sources,
  name_prefix : '',
  vs_module_defs : 'src/d3d11.def',
  cpp_args : ['-DNOMINMAX', '-D_WIN32_WINNT=0x0a00'],
  link_args : cpp.get_id() == 'gcc'
    ? ['-static', '-static-libgcc', '-static-libstdc++', '-Wl,--file-alignment=4096', '-Wl,--strip-all'] : [],
  install : true)
