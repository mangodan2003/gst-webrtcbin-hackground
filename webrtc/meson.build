gst_req = '>= 1.14.0'
gstsdp_dep = dependency('gstreamer-sdp-1.0', version : gst_req,
    fallback : ['gst-plugins-base', 'sdp_dep'])

libsoup_dep = dependency('libsoup-2.4', version : '>=2.48',
    fallback : ['libsoup', 'libsoup_dep'])
json_glib_dep = dependency('json-glib-1.0',
    fallback : ['json-glib', 'json_glib_dep'])

py3_mod = import('python3')
py3 = py3_mod.find_python()

py3_version = py3_mod.language_version()
if py3_version.version_compare('< 3.6')
  error('Could not find a sufficient python version required: 3.6, found {}'.format(py3_version))
endif

subdir('signalling')
subdir('sendrecv')
