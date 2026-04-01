# SonicMesa - ssX XAA Frontend Build Configuration
#
# Copyright 2026 Collin Beyer, AzuriteShift, and ssX Contributors
# SPDX-License-Identifier: ssX

# ssX XAA Frontend Library
# Provides direct-to-metal 2D acceleration via XAA for 5800X3D

ssx_xaa_files = files(
  'ssx_xaa_bridge.h',
  'ssx_xaa_io_uring.h',
  'ssx_xaa.c',
  'ssx_xaa_io_uring.c',
)

# Create static library for ssX XAA frontend
ssx_xaa_deps = [
  dep_libdrm,
  dep_threads,
  dep_uring,
]

ssx_xaa_inc = include_directories('.')

libssx_xaa = static_library(
  'ssx_xaa',
  ssx_xaa_files,
  include_directories: [
    inc_gallium,
    inc_include,
    inc_src,
    ssx_xaa_inc,
  ],
  dependencies: ssx_xaa_deps,
  build_by_default: false,
  install: false,
)

# Export for use by other modules
ssx_xaa_dep = declare_dependency(
  link_with: libssx_xaa,
  include_directories: ssx_xaa_inc,
)
