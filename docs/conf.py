# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sphinx reads its settings from lowercase module-level names
# pylint: disable=invalid-name

"""Sphinx configuration for hipObject documentation.

Sphinx reads this file as it is, with or without CMake, so it has
to find everything it needs from its own directory.
"""

import re
from pathlib import Path

project = "hipObject"
author = "Advanced Micro Devices, Inc."
# pylint: disable=redefined-builtin
copyright = "2025-2026 Advanced Micro Devices, Inc. All rights reserved."
# pylint: enable=redefined-builtin


def _cmake_version():
    """Return the version that the top-level CMakeLists.txt sets."""
    cmake = (Path(__file__).resolve().parent.parent / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    parts = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^set\(HIPOBJ_VERSION_{part} (\d+)\)$", cmake, re.MULTILINE)
        if not match:
            raise ValueError(f"HIPOBJ_VERSION_{part} not found in CMakeLists.txt")
        parts.append(match.group(1))
    return ".".join(parts)


# The project's version, from CMakeLists.txt
version = _cmake_version()
release = version

# -- Extensions ----------------------------------------------

extensions = [
    "breathe",
    # Emits .nojekyll into the build. GitHub Pages serves this
    # site from the gh-pages branch, and without that file it
    # runs Jekyll, which strips _static/ and leaves the site
    # without its CSS.
    "sphinx.ext.githubpages",
]

# -- Breathe (Doxygen XML import) ----------------------------

# Where running doxygen in doxygen/ puts the XML. The CMake build
# writes it to the build directory, and passes that path with
# -D breathe_projects.hipobj=...
breathe_projects = {
    "hipobj": "doxygen/xml",
}
breathe_default_project = "hipobj"
breathe_default_members = ("members",)

# -- General -------------------------------------------------

exclude_patterns = [
    "_build",
    "Thumbs.db",
    ".DS_Store",
]
templates_path = []

# -- HTML output ---------------------------------------------

html_theme = "sphinx_book_theme"
html_theme_options = {
    "repository_url": "https://github.com/ROCm/hipObject",
    "use_repository_button": True,
    "show_toc_level": 2,
    # Banner across the top of every page (the theme inserts
    # it as HTML)
    "announcement": (
        "<strong>hipObject is an experimental library. "
        "It is not intended for production use.</strong>"
    ),
}
html_title = f"hipObject {version}"
html_static_path = []
