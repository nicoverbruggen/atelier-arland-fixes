project('atelier-arland-fixes', ['c', 'cpp'], version : '0.5.0',
  default_options : ['cpp_std=c++17', 'buildtype=release', 'strip=true'])

cpp = meson.get_compiler('cpp')
windows = import('windows')   # compiles the DLL VERSIONINFO resources (windres / rc)
common_cpp_args = ['-DNOMINMAX', '-D_WIN32_WINNT=0x0a00']
common_link_args = cpp.get_id() == 'gcc' ? ['-static', '-static-libgcc', '-static-libstdc++', '-Wl,--strip-all'] : []

if host_machine.cpu_family() == 'x86'
  shared_library('msimg32',
    ['src/launcher_proxy.cpp', windows.compile_resources('src/msimg32.rc')],
    name_prefix : '',
    vs_module_defs : 'src/msimg32.def',
    cpp_args : common_cpp_args,
    link_args : common_link_args,
    install : true)
else
  # The bundled replacement fonts are embedded as C++ byte arrays generated at
  # build time from the vendored .ttf files (scripts/embed_font.py), so the large
  # generated sources are not checked in. Declarations live in src/embedded_font.h.
  py = find_program('python3')
  embed_font = files('scripts/embed_font.py')
  embedded_cuprum = custom_target('embedded_font_cuprum',
    input : 'vendor/font/Cuprum-Medium.ttf',
    output : 'embedded_font_cuprum.cpp',
    command : [py, embed_font, '@INPUT@', '@OUTPUT@', 'kEmbeddedFontCuprum'])
  embedded_np = custom_target('embedded_font_np',
    input : 'vendor/font/NationalPark-Medium.ttf',
    output : 'embedded_font_np.cpp',
    command : [py, embed_font, '@INPUT@', '@OUTPUT@', 'kEmbeddedFontNationalPark'])

  sources = files(
    'src/main.cpp',
    'src/game.cpp',
    'src/config.cpp',
    'src/crash_log.cpp',
    'src/smaa.cpp',
    'src/font_hires.cpp',
    'src/menu_fix.cpp',
    'src/battle_shadow_restore.cpp',
    'src/sync_fix.cpp',
    'src/battle_shadows.cpp',
    'src/hook_util.cpp',
    'vendor/minhook/src/hde/hde64.c',
    'vendor/minhook/src/hook.c',
    'vendor/minhook/src/buffer.c',
    'vendor/minhook/src/trampoline.c',
  )

  shared_library('d3d11',
    [sources, embedded_cuprum, embedded_np,
     windows.compile_resources('src/d3d11.rc')],
    name_prefix : '',
    vs_module_defs : 'src/d3d11.def',
    include_directories : include_directories('src'),  # so generated fonts find embedded_font.h
    cpp_args : common_cpp_args,
    link_args : common_link_args + (cpp.get_id() == 'gcc' ? ['-Wl,--file-alignment=4096'] : []),
    install : true)
endif
