# SPDX-FileCopyrightText: 2020-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Initialization script for 2D Animation template

import bpy
from bpy.app.handlers import persistent


def update_factory_startup_screens():
    # 2D Animation.
    screen = bpy.data.screens.get("2D Animation")
    if screen:
        for area in screen.areas:
            if area.type == 'PROPERTIES':
                # Set Tool settings as default in properties panel.
                space = area.spaces.active
                space.context = 'TOOL'
            elif area.type == 'DOPESHEET_EDITOR':
                # Open sidebar in Dope-sheet.
                space = area.spaces.active
                space.show_region_ui = True
            elif area.type == 'VIEW_3D':
                space = area.spaces.active
                space.overlay.use_gpencil_grid = True
                space.overlay.gpencil_grid_opacity = 0.5
                space.overlay.show_floor = True
                space.overlay.show_ortho_grid = True
                space.overlay.show_axis_x = True
                space.overlay.show_axis_y = True
                if space.region_3d.view_perspective == 'CAMERA':
                    space.region_3d.view_rotation = (1.0, 0.0, 0.0, 0.0)

    # 2D Full Canvas.
    screen = bpy.data.screens.get("2D Full Canvas")
    if screen:
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                space = area.spaces.active
                space.shading.type = 'MATERIAL'
                space.shading.use_scene_world = True
                space.overlay.use_gpencil_grid = True
                space.overlay.gpencil_grid_opacity = 0.5
                space.overlay.show_floor = True
                space.overlay.show_ortho_grid = True
                space.overlay.show_axis_x = True
                space.overlay.show_axis_y = True
                if space.region_3d.view_perspective == 'CAMERA':
                    space.region_3d.view_rotation = (1.0, 0.0, 0.0, 0.0)


def update_factory_startup_scenes():
    for scene in bpy.data.scenes:
        scene.tool_settings.use_keyframe_insert_auto = True
        scene.tool_settings.gpencil_sculpt.use_scale_thickness = True
        scene.tool_settings.gpencil_sculpt.lock_axis = 'AXIS_Z'
        scene.tool_settings.plane_axis = 'Z'
        scene.tool_settings.gpencil_stroke_placement_view3d = 'ORIGIN'
        if scene.camera:
            scene.camera.location = (0.0, 0.0, 12.0)
            scene.camera.rotation_euler = (0.0, 0.0, 0.0)


def update_factory_startup_grease_pencils():
    for grease_pencil in bpy.data.grease_pencils:
        grease_pencil.onion_keyframe_type = 'ALL'


@persistent
def load_handler(_):
    update_factory_startup_screens()
    update_factory_startup_scenes()
    update_factory_startup_grease_pencils()


def register():
    bpy.app.handlers.load_factory_startup_post.append(load_handler)


def unregister():
    bpy.app.handlers.load_factory_startup_post.remove(load_handler)
