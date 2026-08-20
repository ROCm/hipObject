# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Sphinx configuration for hipObject documentation."""

project = "hipObject"
author = "Advanced Micro Devices, Inc."
copyright = (
    "2025-2026 Advanced Micro Devices, Inc. "
    "All rights reserved."
)

version = "0.1.0"
release = version

# -- Extensions ----------------------------------------------

extensions = [
    "breathe",
]

# -- Breathe (Doxygen XML import) ----------------------------

breathe_projects = {
    "hipobj": "@BREATHE_DOC_XML_DIR@",
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
    "repository_url": (
        "https://github.com/ROCm/hipObject"
    ),
    "use_repository_button": True,
    "show_toc_level": 2,
}
html_title = f"hipObject {version}"
html_static_path = []
