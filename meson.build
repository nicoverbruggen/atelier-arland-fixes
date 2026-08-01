project('atelier-arland-fixes', ['c', 'cpp'],
  version : run_command(
    'python3', 'scripts/read_version.py', 'VERSION',
    check : true).stdout().strip(),
  default_options : ['cpp_std=c++17', 'buildtype=release', 'strip=true'])

cpp = meson.get_compiler('cpp')
windows = import('windows')   # compiles the DLL VERSIONINFO resources (windres / rc)
common_cpp_args = ['-DNOMINMAX', '-D_WIN32_WINNT=0x0a00']
common_link_args = cpp.get_id() == 'gcc' ? ['-static', '-static-libgcc', '-static-libstdc++', '-Wl,--strip-all'] : []

version_parts = meson.project_version().split('.')
if version_parts.length() != 3
  error('VERSION must contain three numeric components: X.Y.Z')
endif
version_data = configuration_data()
version_data.set('VERSION_COMMA', ','.join(version_parts) + ',0')
version_data.set_quoted('VERSION_STRING', meson.project_version() + '.0')
version_data.set_quoted('VERSION_SHORT', meson.project_version())
version_rc = configure_file(
  output : 'version.rc',
  configuration : version_data)
version_header = configure_file(
  input : 'src/version.h.in',
  output : 'version.h',
  configuration : version_data)
resource_includes = include_directories('.')

if host_machine.cpu_family() == 'x86'
  shared_library('msimg32',
    ['src/launcher_proxy.cpp',
     windows.compile_resources(
       'src/msimg32.rc',
       depend_files : version_rc,
       include_directories : resource_includes)],
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
  embedded_np = custom_target('embedded_font_np_semibold',
    input : 'vendor/font/NationalPark-SemiBold.ttf',
    output : 'embedded_font_np_semibold.cpp',
    command : [py, embed_font, '@INPUT@', '@OUTPUT@', 'kEmbeddedFontNationalParkSemiBold'])
  embedded_nunito = custom_target('embedded_font_nunito',
    input : 'vendor/font/Nunito-Regular.ttf',
    output : 'embedded_font_nunito.cpp',
    command : [py, embed_font, '@INPUT@', '@OUTPUT@', 'kEmbeddedFontNunito'])
  embedded_mgopen = custom_target('embedded_font_mgopen',
    input : 'vendor/font/Cosmetica-Medium.ttf',
    output : 'embedded_font_mgopen.cpp',
    command : [py, embed_font, '@INPUT@', '@OUTPUT@', 'kEmbeddedFontMgOpenCosmetica'])

  # DXGI interface GUIDs (IID_IDXGIFactory, IID_IDXGIDevice). MSVC needs
  # dxguid.lib explicitly; MinGW happens to resolve them elsewhere.
  dxguid_dep = cpp.find_library('dxguid')

  sources = files(
    'src/main.cpp',
    'src/game.cpp',
    'src/config.cpp',
    'src/crash_log.cpp',
    'src/smaa.cpp',
    'src/field_physics.cpp',
    'src/worldmap_fix.cpp',
    'src/item_guard.cpp',
    'src/shop_fix.cpp',
    'src/supersample.cpp',
    'src/font_hires.cpp',
    'src/menu_fix.cpp',
    'src/battle_shadow_restore.cpp',
    'src/sync_fix.cpp',
    'src/battle_shadows.cpp',
    'src/window_mode.cpp',
    'src/window_title.cpp',
    'src/hook_util.cpp',
    'vendor/minhook/src/hde/hde64.c',
    'vendor/minhook/src/hook.c',
    'vendor/minhook/src/buffer.c',
    'vendor/minhook/src/trampoline.c',
  )

  shared_library('d3d11',
    [sources, embedded_np, embedded_nunito, embedded_mgopen,
     windows.compile_resources(
       'src/d3d11.rc',
       depend_files : version_rc,
       include_directories : resource_includes)],
    name_prefix : '',
    vs_module_defs : 'src/d3d11.def',
    include_directories : include_directories('src'),  # so generated fonts find embedded_font.h
    dependencies : dxguid_dep,
    cpp_args : common_cpp_args,
    link_args : common_link_args + (cpp.get_id() == 'gcc' ? ['-Wl,--file-alignment=4096'] : []),
    install : true)

  # Standalone 64-bit GUI to view and edit arland-fix.ini (src/config_gui/).
  # Own target, own sources; shares no object files with the DLL. The
  # win_subsystem kwarg selects the GUI subsystem (WinMain) for both MinGW
  # (-mwindows) and MSVC (/SUBSYSTEM:WINDOWS).
  executable('arland-fix-launcher',
    ['src/config_gui/main.cpp',
     # Carries the ComCtl32 v6 manifest, which is what gets the themed common
     # controls rather than the flat 5.82 ones. See the manifest for the rest.
     windows.compile_resources(
       'src/config_gui/arland-fix-launcher.rc',
       depend_files : [
         version_rc,
         files('src/config_gui/arland-fix-launcher.manifest'),
       ],
       include_directories : resource_includes)],
    win_subsystem : 'windows',
    # No embedded font here: the launcher draws in the platform's own UI face,
    # so it carries no typeface of its own. See createUiFont.
    include_directories : include_directories('src'),
    dependencies : [
      cpp.find_library('comctl32'),
      # OpenThemeData/DrawThemeBackground, for sampling the visual style's tab
      # page colour so the controls on it match. See themedTabBodyColor.
      cpp.find_library('uxtheme'),
      cpp.find_library('shell32'),
      # GetFileVersionInfo, for reading the installed mod's version off d3d11.dll
      cpp.find_library('version'),
      cpp.find_library('ole32'),
    ],
    cpp_args : common_cpp_args,
    link_args : common_link_args,
    install : true)
endif
