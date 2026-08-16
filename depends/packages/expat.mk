package=expat
$(package)_version=2.6.2
$(package)_download_path=https://github.com/libexpat/libexpat/releases/download/R_2_6_2
$(package)_file_name=$(package)-$($(package)_version).tar.bz2
$(package)_sha256_hash=9c7c1b5dcbc3c237c500a8fb1493e14d9582146dd9b42aa8d3ffb856a3b927e0

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf share lib/cmake lib/*.la
endef
