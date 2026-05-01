import bpy, mathutils, shutil, math

# Quaternion derived from: identity -> orbit right 45deg (around Y) -> tilt down 30deg (around local X)
# Both operations preserve level horizon in Y-up world.
q_orbit = mathutils.Quaternion((math.cos(math.radians(22.5)), 0, math.sin(math.radians(22.5)), 0))
q_tilt  = mathutils.Quaternion((math.cos(math.radians(15)), math.sin(math.radians(15)), 0, 0))
view_quat = q_orbit @ q_tilt

print(f"quat: w={view_quat.w:.6f} x={view_quat.x:.6f} y={view_quat.y:.6f} z={view_quat.z:.6f}")
print(f"Level check (wz+xy): {view_quat.w*view_quat.z + view_quat.x*view_quat.y:.8f}")

for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            for space in area.spaces:
                if space.type == 'VIEW_3D':
                    r3d = space.region_3d
                    r3d.view_perspective = 'PERSP'
                    r3d.view_rotation = view_quat
                    r3d.view_distance = 8.0
                    ov = space.overlay
                    ov.show_floor = True
                    ov.show_axis_x = True
                    ov.show_axis_y = True
                    ov.show_axis_z = True

bpy.ops.wm.save_homefile()
shutil.copy2(
    r"C:\Users\franc\AppData\Roaming\Blender Foundation\Blender\5.2\config\startup.blend",
    r"d:\blender-build\blender\release\datafiles\startup.blend"
)
print("Done")
