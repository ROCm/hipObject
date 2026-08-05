# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sphinx + Breathe + Doxygen documentation pipeline
# (following ROCm best practices)
#
# A Python venv is created automatically in the build
# tree and populated from requirements.txt.

option(HIPOBJ_BUILD_DOCS
  "Build documentation (Sphinx + Breathe + Doxygen)"
  OFF)

if(HIPOBJ_BUILD_DOCS)
  find_package(Doxygen QUIET)
  if(NOT DOXYGEN_FOUND)
    message(FATAL_ERROR
      "Doxygen not found but HIPOBJ_BUILD_DOCS is ON.\n"
      "Install with:\n"
      "  sudo apt install doxygen   (Debian/Ubuntu)\n"
      "  sudo dnf install doxygen   (Fedora/RHEL)")
  endif()

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(NOT Python3_FOUND)
    message(FATAL_ERROR
      "Python3 not found but HIPOBJ_BUILD_DOCS is ON.\n"
      "Install with:\n"
      "  sudo apt install python3 python3-venv"
      "   (Debian/Ubuntu)\n"
      "  sudo dnf install python3   (Fedora/RHEL)")
  endif()

  # ── Python venv with Sphinx + Breathe ────────────────
  set(HIPOBJ_DOCS_VENV
    "${CMAKE_BINARY_DIR}/docs-venv")
  set(HIPOBJ_DOCS_VENV_STAMP
    "${HIPOBJ_DOCS_VENV}/stamp")
  set(HIPOBJ_DOCS_REQUIREMENTS
    "${CMAKE_SOURCE_DIR}/requirements.txt")

  if(WIN32)
    set(HIPOBJ_VENV_BIN
      "${HIPOBJ_DOCS_VENV}/Scripts")
  else()
    set(HIPOBJ_VENV_BIN
      "${HIPOBJ_DOCS_VENV}/bin")
  endif()

  set(SPHINX_BUILD
    "${HIPOBJ_VENV_BIN}/sphinx-build")

  add_custom_command(
    OUTPUT ${HIPOBJ_DOCS_VENV_STAMP}
    COMMAND ${Python3_EXECUTABLE}
      -m venv ${HIPOBJ_DOCS_VENV}
    COMMAND ${HIPOBJ_VENV_BIN}/pip install
      --quiet --upgrade pip
    COMMAND ${HIPOBJ_VENV_BIN}/pip install
      --quiet -r ${HIPOBJ_DOCS_REQUIREMENTS}
    COMMAND ${CMAKE_COMMAND} -E touch
      ${HIPOBJ_DOCS_VENV_STAMP}
    DEPENDS ${HIPOBJ_DOCS_REQUIREMENTS}
    COMMENT "Creating docs venv and installing deps"
    VERBATIM
  )

  add_custom_target(docs-venv
    DEPENDS ${HIPOBJ_DOCS_VENV_STAMP}
  )

  # ── Paths ────────────────────────────────────────────
  set(HIPOBJ_DOC_PATH
    "${CMAKE_BINARY_DIR}/docs")
  set(BREATHE_DOC_XML_DIR
    "${HIPOBJ_DOC_PATH}/xml")

  set(HIPOBJ_DOXYFILE_INPUT
    "${CMAKE_SOURCE_DIR}/include")

  # Configure Doxyfile (substitutes @VARIABLES@)
  configure_file(
    ${CMAKE_SOURCE_DIR}/docs/Doxyfile.in
    ${CMAKE_BINARY_DIR}/Doxyfile
    @ONLY
  )

  # Configure conf.py (substitutes Breathe XML path)
  configure_file(
    ${CMAKE_SOURCE_DIR}/docs/conf.py
    ${CMAKE_BINARY_DIR}/docs-sphinx/conf.py
    @ONLY
  )

  # ── Doxygen target: source headers -> XML ────────────
  add_custom_target(doxygen
    COMMAND ${DOXYGEN_EXECUTABLE}
      ${CMAKE_BINARY_DIR}/Doxyfile
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating Doxygen XML"
    VERBATIM
  )

  # ── Sphinx target: RST + Doxygen XML -> HTML ─────────
  # Target name follows ROCm convention (ROCMSphinxDoc)
  add_custom_target(sphinx-html
    COMMAND ${SPHINX_BUILD}
      -b html
      -c ${CMAKE_BINARY_DIR}/docs-sphinx
      ${CMAKE_SOURCE_DIR}/docs
      ${HIPOBJ_DOC_PATH}/html
    DEPENDS doxygen docs-venv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building Sphinx HTML documentation"
    VERBATIM
  )
endif()
