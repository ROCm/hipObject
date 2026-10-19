# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sphinx + Breathe + Doxygen documentation pipeline
# (following ROCm best practices)
#
# A Python venv is created automatically in the build
# tree and populated from docs/sphinx/requirements.txt.
#
# docs/doxygen/Doxyfile and docs/conf.py work without
# CMake too, as Read the Docs uses them, so nothing here
# configures them. The build only sends the output to the
# build tree.

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
    "${PROJECT_BINARY_DIR}/docs-venv")
  set(HIPOBJ_DOCS_VENV_STAMP
    "${HIPOBJ_DOCS_VENV}/stamp")
  set(HIPOBJ_DOCS_REQUIREMENTS
    "${PROJECT_SOURCE_DIR}/docs/sphinx/requirements.txt")

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

  add_custom_target(hipobj-docs-venv
    DEPENDS ${HIPOBJ_DOCS_VENV_STAMP}
  )

  # ── Paths ────────────────────────────────────────────
  set(HIPOBJ_DOC_PATH
    "${PROJECT_BINARY_DIR}/docs")
  set(HIPOBJ_DOXYGEN_DIR
    "${PROJECT_SOURCE_DIR}/docs/doxygen")

  # A Doxyfile that reads the committed one and changes
  # only where the output goes. Doxygen still has to run
  # in docs/doxygen, which the committed file's relative
  # paths are from.
  file(CONFIGURE
    OUTPUT ${PROJECT_BINARY_DIR}/Doxyfile
    CONTENT [[
@INCLUDE               = "@HIPOBJ_DOXYGEN_DIR@/Doxyfile"
OUTPUT_DIRECTORY       = "@HIPOBJ_DOC_PATH@"
]]
    @ONLY
  )

  # ── Doxygen target: source headers -> XML ────────────
  add_custom_target(hipobj-doxygen
    COMMAND ${DOXYGEN_EXECUTABLE}
      ${PROJECT_BINARY_DIR}/Doxyfile
    WORKING_DIRECTORY ${HIPOBJ_DOXYGEN_DIR}
    COMMENT "Generating Doxygen XML"
    VERBATIM
  )

  # ── Sphinx target: RST + Doxygen XML -> HTML ─────────
  # -d keeps Sphinx's doctree cache out of the HTML
  # directory, which is published to GitHub Pages as is.
  # -D points Breathe at the XML in the build tree.
  add_custom_target(hipobj-sphinx-html
    COMMAND ${SPHINX_BUILD}
      -b html
      -d ${HIPOBJ_DOC_PATH}/doctrees
      -D breathe_projects.hipobj=${HIPOBJ_DOC_PATH}/xml
      ${PROJECT_SOURCE_DIR}/docs
      ${HIPOBJ_DOC_PATH}/html
    DEPENDS hipobj-doxygen hipobj-docs-venv
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Building Sphinx HTML documentation"
    VERBATIM
  )
endif()
