"""Shared 3D scene for the lamp renders and the assembly video (pyrender,
software OpenGL via OSMesa). Parts come straight from ../lamp.py."""
import math
import os
import pathlib
import sys

os.environ.setdefault("PYOPENGL_PLATFORM", "osmesa")
import numpy as np
import pyrender
import trimesh
from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import lamp  # noqa: E402

GRAPHITE = [0.10, 0.105, 0.115, 1.0]
ORANGE = [1.0, 0.42, 0.08, 1.0]
BONE = [0.86, 0.84, 0.80, 1.0]
COLORS = {
    "base": GRAPHITE, "base_lid": GRAPHITE, "jam_nut": ORANGE, "shoulder": GRAPHITE,
    "lower_arm": GRAPHITE, "upper_arm": GRAPHITE, "knob": ORANGE, "head": GRAPHITE,
    "shade": ORANGE, "washer": BONE, "shim_1mm": BONE, "shim_2mm": BONE, "cable_clip": ORANGE, "fit_coupon": BONE,
}

_cache = {}


def part_mesh(name):
    key = "knob" if name.startswith("knob") else name
    if key not in _cache:
        fn = lamp.PARTS[key][0]
        tm = lamp.to_trimesh(fn())
        tm.merge_vertices()
        tm = tm.smooth_shaded if len(tm.faces) < 0 else tm   # keep flat facets on threads
        _cache[key] = tm
    return _cache[key]


def material(rgba, rough=0.55, metal=0.0):
    return pyrender.MetallicRoughnessMaterial(baseColorFactor=rgba, roughnessFactor=rough,
                                              metallicFactor=metal)


def screen_disc(image_path, radius=16.2, n=96):
    """A textured disc (the display) in head coordinates, facing −Z."""
    # pre-rotated: the disc UVs put image-up on local +Y; the head's "up" is +X
    img = Image.open(image_path).convert("RGB").resize((256, 256)).rotate(-90)
    # round mask: the panel is a circle
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse([0, 0, 255, 255], fill=255)
    black = Image.new("RGB", img.size, (0, 0, 0))
    img = Image.composite(img, black, mask)
    verts = [[0, 0, 0]]
    uv = [[0.5, 0.5]]
    for k in range(n):
        a = 2 * math.pi * k / n
        verts.append([radius * math.cos(a), radius * math.sin(a), 0])
        uv.append([0.5 + 0.5 * math.cos(a), 0.5 - 0.5 * math.sin(a)])
    faces = [[0, 1 + (k + 1) % n, 1 + k] for k in range(n)]      # normal toward −Z
    tm = trimesh.Trimesh(np.array(verts), np.array(faces), process=False)
    tex = trimesh.visual.TextureVisuals(uv=np.array(uv), image=img)
    tm.visual = tex
    mat = pyrender.MetallicRoughnessMaterial(baseColorTexture=pyrender.Texture(source=np.asarray(img),
                                             source_channels="RGB"),
                                             emissiveTexture=pyrender.Texture(source=np.asarray(img),
                                             source_channels="RGB"),
                                             emissiveFactor=[1.0, 1.0, 1.0], roughnessFactor=0.2)
    return pyrender.Mesh.from_trimesh(tm, material=mat, smooth=False)


def look_at(eye, target, up=(0, 0, 1)):
    eye, target, up = map(np.asarray, (eye, target, up))
    f = target - eye
    f = f / np.linalg.norm(f)
    r = np.cross(f, up)
    r = r / np.linalg.norm(r)
    u = np.cross(r, f)
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = r, u, -f, eye
    return m


class Stage:
    def __init__(self, w=1280, h=720):
        self.w, self.h = w, h
        self.renderer = pyrender.OffscreenRenderer(w, h)

    def render(self, parts, cam_eye, cam_target, screen=None, screen_tf=None, desk=True,
               screen_light=True, fov=0.62, bg=(0.035, 0.038, 0.045)):
        """parts: [(name, 4×4 world transform)]; screen: png path shown on the display."""
        sc = pyrender.Scene(bg_color=[*bg, 1.0], ambient_light=[0.10, 0.10, 0.11])
        for name, tf in parts:
            col = COLORS.get(name.split("#")[0].replace("knob_shoulder", "knob").replace(
                "knob_elbow", "knob").replace("knob_head", "knob"), GRAPHITE)
            rough = 0.35 if col is ORANGE else 0.62
            sc.add(pyrender.Mesh.from_trimesh(part_mesh(name.split("#")[0]), material=material(col, rough),
                                              smooth=False), pose=tf)
        if desk:
            plane = trimesh.creation.box([900, 900, 2])
            sc.add(pyrender.Mesh.from_trimesh(plane, material=material([0.16, 0.13, 0.11, 1.0], 0.8)),
                   pose=trimesh.transformations.translation_matrix([0, 0, -1.0]))
        if screen is not None and screen_tf is not None:
            glass = trimesh.creation.cylinder(radius=18.4, height=0.6, sections=96)
            sc.add(pyrender.Mesh.from_trimesh(glass, material=material([0.01, 0.01, 0.012, 1], 0.1)),
                   pose=screen_tf @ trimesh.transformations.translation_matrix([0, 0, 0.6]))
            sc.add(screen_disc(screen), pose=screen_tf @ trimesh.transformations.translation_matrix([0, 0, 0.25]))
            if screen_light:   # the "bulb" lights the desk
                spot = pyrender.SpotLight(color=[0.55, 0.7, 1.0], intensity=30000.0,
                                          innerConeAngle=0.35, outerConeAngle=0.95)
                flip = trimesh.transformations.rotation_matrix(math.pi, [1, 0, 0])   # spot looks down −Z
                sc.add(spot, pose=screen_tf @ trimesh.transformations.translation_matrix([0, 0, -2]) @ flip @ flip)
        cam = pyrender.PerspectiveCamera(yfov=fov, aspectRatio=self.w / self.h, znear=5, zfar=3000)
        sc.add(cam, pose=look_at(cam_eye, cam_target))
        key = look_at(np.array(cam_eye) + np.array([120, -260, 420]), cam_target)
        sc.add(pyrender.DirectionalLight(color=[1.0, 0.97, 0.92], intensity=3.2), pose=key)
        fill = look_at(np.array([-300, 250, 150]), cam_target)
        sc.add(pyrender.DirectionalLight(color=[0.6, 0.7, 1.0], intensity=1.0), pose=fill)
        rim = look_at(np.array([-250, -50, 350]), cam_target)
        sc.add(pyrender.DirectionalLight(color=[1.0, 0.6, 0.35], intensity=1.2), pose=rim)
        color, _ = self.renderer.render(sc, flags=pyrender.RenderFlags.SHADOWS_DIRECTIONAL)
        return Image.fromarray(color)


def assembled(pose=None):
    P = pose or lamp.pose()
    parts = [(n, tf) for n, tf in P.items() if not n.startswith("_")]
    return parts, P


def screen_tf(P):
    return P["head"] @ trimesh.transformations.translation_matrix([0, 0, lamp.GLASS_Z])
