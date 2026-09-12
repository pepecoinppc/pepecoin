package=xcb_proto
$(package)_version=1.10
$(package)_download_path=http://xcb.freedesktop.org/dist
$(package)_file_name=xcb-proto-$($(package)_version).tar.bz2
$(package)_sha256_hash=7ef40ddd855b750bc597d2a435da21e55e502a0fefa85b274f2c922800baaf05

define $(package)_set_vars
  $(package)_config_opts=--disable-shared
  $(package)_config_opts_linux=--with-pic
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_preprocess_cmds
  sed -i.old \
    -e 's/import sys, os, py_compile, imp/import sys, os, py_compile, importlib.util/' \
    -e "s/if hasattr(imp, 'get_tag'):/if True:/" \
    -e 's/imp\.cache_from_source(filepath, False)/importlib.util.cache_from_source(filepath, optimization=1)/' \
    -e 's/imp\.cache_from_source(filepath)/importlib.util.cache_from_source(filepath)/' \
  py-compile
endef

define $(package)_postprocess_cmds
  find -name "*.pyc" -delete && \
  find -name "*.pyo" -delete
endef
