project('atelier-arland-fixes', ['c', 'cpp'], version : '0.4.0',
  default_options : ['cpp_std=c++17', 'buildtype=release', 'strip=true'])

cpp = meson.get_compiler('cpp')
common_cpp_args = ['-DNOMINMAX', '-D_WIN32_WINNT=0x0a00']
common_link_args = cpp.get_id() == 'gcc' ? ['-static', '-static-libgcc', '-static-libstdc++', '-Wl,--strip-all'] : []

if host_machine.cpu_family() == 'x86'
  shared_library('msimg32', 'src/launcher_proxy.cpp',
    name_prefix : '',
    vs_module_defs : 'src/msimg32.def',
    cpp_args : common_cpp_args,
    link_args : common_link_args,
    install : true)
else
  sources = files(
    'src/main.cpp',
    'src/game.cpp',
    'src/config.cpp',
    'src/crash_log.cpp',
    'src/smaa.cpp',
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
    cpp_args : common_cpp_args,
    link_args : common_link_args + (cpp.get_id() == 'gcc' ? ['-Wl,--file-alignment=4096'] : []),
    install : true)
endif
