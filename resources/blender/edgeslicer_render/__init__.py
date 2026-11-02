# SPDX-License-Identifier: GPL-3.0-or-later
#
# EdgeSlicer Render - render EdgeSlicer Showcase scenes in Blender's Cycles.
# Copyright (C) 2026 the EdgeSlicer authors.
#
# This program is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this program. If
# not, see <https://www.gnu.org/licenses/>.
#
# What it does (tests/design_showcase_realism.md section 5.3): reads an "edgeslicer-blender-package"
# (a folder or a zip written by EdgeSlicer's Showcase tab) and builds a ready-to-render Cycles
# scene from it: the parts with a node setup per finish, the HDRI world, the camera with its depth of
# field, the key / fill / rim lights, the floor or a shadow catcher over the backdrop or photo, the
# video's camera path, and the colour management matching Showcase's tone curve. One panel in the
# 3D view's sidebar (N): "EdgeSlicer".
#
# It is a separate program, written for Blender; the package format is the only interface with
# EdgeSlicer. Blender 4.2 LTS and newer (as an extension); 3.6 works as a legacy add-on.

bl_info = {
    "name": "EdgeSlicer Render",
    "author": "EdgeSlicer",
    "version": (1, 1, 1),
    "blender": (3, 6, 0),
    "location": "View3D > Sidebar > EdgeSlicer, File > Import > EdgeSlicer Showcase package",
    "description": "Render EdgeSlicer Showcase scenes in Cycles",
    "category": "Import-Export",
}

ADDON_VERSION = bl_info["version"]

if "bpy" in locals():
    import importlib

    for _m in (compat, package, materials, scene, importer, panel):  # noqa: F821
        importlib.reload(_m)
else:
    from . import compat, package, materials, scene, importer, panel

import bpy  # noqa: E402


def register():
    panel.register()


def unregister():
    panel.unregister()
