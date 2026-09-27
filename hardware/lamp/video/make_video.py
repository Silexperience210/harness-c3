#!/usr/bin/env python3
"""Assembly & setup video for the Harness C3 lamp, in French and English.

    python3 make_video.py render      # 3D frames (once, language-independent)
    python3 make_video.py compose     # voice-over (Piper), captions, MP4 ×2

Needs: pyrender + OSMesa, Pillow, ffmpeg, piper-tts with the voices
fr_FR-siwis-medium and en_US-lessac-medium in $VOICES (default /tmp/voices).
"""
import json
import math
import os
import pathlib
import subprocess
import sys
import wave

os.environ.setdefault("PYOPENGL_PLATFORM", "osmesa")
import numpy as np
import pyrender
import trimesh
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import scene  # noqa: E402
from scene import lamp  # noqa: E402

W, H = 1024, 576          # 3D render size (upscaled to 1280×720 at encode)
FPS = 15
WORK = pathlib.Path(os.environ.get("WORK", "/tmp/lampvideo"))
VOICES = pathlib.Path(os.environ.get("VOICES", "/tmp/voices"))
SIM = HERE.parents[2] / "firmware" / "test" / "sim" / "out"
FONT_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
TR = trimesh.transformations

# ── the script ──────────────────────────────────────────────────────────────
SCENES = [
    dict(id="intro", min=6, title={"fr": "Harness C3 — la lampe", "en": "Harness C3 — the lamp"},
         say={"fr": "Harness C3 : une petite lampe de bureau dont l'ampoule est l'écran tactile rond de votre cadran d'agents.",
              "en": "Harness C3: a small desk lamp whose bulb is the round touch screen of your agent dial."}),
    dict(id="parts", min=8, title={"fr": "13 pièces, sans support", "en": "13 parts, no supports"},
         say={"fr": "Treize pièces à imprimer sans support, en PLA ou PETG. Commencez par la pièce de test, et prévoyez la carte, un câble USB-A vers USB-C et un peu de lest.",
              "en": "Thirteen parts, printed without supports, in PLA or PETG. Start with the fit coupon, and have the board, a USB-A to USB-C cable and some ballast at hand."}),
    dict(id="step1", min=6, step=1, title={"fr": "Le pied", "en": "The base"},
         say={"fr": "Étape un : remplissez le pied de lest, billes, écrous ou pièces, puis vissez le couvercle par-dessous.",
              "en": "Step one: fill the base with ballast, steel balls, nuts or coins, then screw the lid on from below."}),
    dict(id="step2", min=7, step=2, title={"fr": "L'épaule", "en": "The shoulder"},
         say={"fr": "Étape deux : enfilez le contre-écrou sur la tige, vissez la tige dans le pied, orientez l'épaule, puis bloquez-la avec le contre-écrou.",
              "en": "Step two: thread the jam nut onto the stud, screw the stud into the base, aim the shoulder, then lock it with the jam nut."}),
    dict(id="step3", min=6, step=3, title={"fr": "Bras inférieur", "en": "Lower arm"},
         say={"fr": "Étape trois : posez le bras inférieur contre l'épaule et vissez une molette à travers l'épaule.",
              "en": "Step three: lay the lower arm against the shoulder and drive a knob through the shoulder into it."}),
    dict(id="step4", min=6, step=4, title={"fr": "Le coude", "en": "The elbow"},
         say={"fr": "Étape quatre : même principe au coude, avec le bras supérieur et la deuxième molette.",
              "en": "Step four: same at the elbow, with the upper arm and the second knob."}),
    dict(id="step5", min=8, step=5, title={"fr": "L'écran dans la tête", "en": "Screen into the head"},
         say={"fr": "Étape cinq : glissez la carte dans la tête, port USB-C face à la fenêtre du dessous. Ajoutez une cale si elle bouge, puis vissez l'abat-jour : c'est lui qui tient l'écran.",
              "en": "Step five: slide the board into the head, USB-C port facing the window underneath. Add a shim if it rattles, then screw the shade in: it holds the screen."}),
    dict(id="step6", min=8, step=6, title={"fr": "Tête et câble", "en": "Head and cable"},
         say={"fr": "Étape six : fixez la tête au bras supérieur avec la dernière molette, branchez le câble, et clipsez-le le long des bras jusqu'à la gorge du pied.",
              "en": "Step six: attach the head to the upper arm with the last knob, plug in the cable, and clip it along the arms down to the channel in the base."}),
    dict(id="flash", min=8, title={"fr": "Flasher le firmware", "en": "Flash the firmware"},
         say={"fr": "Flashez le firmware depuis le web flasher, dans Chrome ou Edge : Connect and Flash, puis le port USB JTAG. Pas de port ? Maintenez BOOT en branchant.",
              "en": "Flash the firmware from the web flasher, in Chrome or Edge: Connect and Flash, then the USB JTAG port. No port? Hold BOOT while plugging in."}),
    dict(id="connect", min=8, title={"fr": "Connexion", "en": "Connect"},
         say={"fr": "Branchez la lampe sur l'ordinateur qui fait tourner Harness : vos agents apparaissent. Sous Linux, lancez le pont tools, dial-linux.",
              "en": "Plug the lamp into the computer running Harness: your agents appear. On Linux, start the bridge in tools, dial-linux."}),
    dict(id="use", min=10, title={"fr": "Utilisation", "en": "Using it"},
         say={"fr": "Balayez pour changer d'agent, touchez pour l'ouvrir, répondez aux questions d'un doigt, tirez vers le bas pour les réglages. Desserrez une molette pour orienter la lampe, resserrez pour la bloquer.",
              "en": "Swipe to change agent, tap to open it, answer questions with a finger, pull down for settings. Loosen a knob to aim the lamp, tighten it to lock."}),
    dict(id="outro", min=5, title={"fr": "À vous !", "en": "Your turn!"},
         say={"fr": "Fichiers et code source sur GitHub : Silexperience210, harness-c3.",
              "en": "Files and source code on GitHub: Silexperience210, harness-c3."}),
]
CAPTION = {  # short on-screen text (the voice says the long version)
    "parts": {"fr": ["PLA/PETG · 0,2 mm · 4 périmètres · sans support", "Imprimez d'abord « fit_coupon »",
                     "+ ESP32-2424S012C(-I) · câble USB-A → USB-C · ~150 g de lest"],
              "en": ["PLA/PETG · 0.2 mm · 4 walls · no supports", "Print \"fit_coupon\" first",
                     "+ ESP32-2424S012C(-I) · USB-A → USB-C cable · ~150 g ballast"]},
    "flash": {"fr": ["silexperience210.github.io/harness-c3/webflasher", "Chrome / Edge → « Connect & Flash »",
                     "Port : USB JTAG/serial · pas de port ? maintenir BOOT en branchant"],
              "en": ["silexperience210.github.io/harness-c3/webflasher", "Chrome / Edge → \"Connect & Flash\"",
                     "Port: USB JTAG/serial · no port? hold BOOT while plugging in"]},
    "use": {"fr": ["← → changer d'agent · toucher = ouvrir", "Questions : touchez puis ✓ · ↓ réglages · ↑ défilement",
                   "Molette desserrée = orienter · serrée = bloquer"],
            "en": ["← → change agent · tap = open", "Questions: tap then ✓ · ↓ settings · ↑ scroll",
                   "Knob loose = aim · tight = lock"]},
    "outro": {"fr": ["github.com/Silexperience210/harness-c3"], "en": ["github.com/Silexperience210/harness-c3"]},
    "connect": {"fr": ["Harness sur l'ordinateur → vos agents s'affichent", "Linux : tools/dial-linux (pont local)"],
                "en": ["Harness on the computer → your agents appear", "Linux: tools/dial-linux (local bridge)"]},
}


# ── 3D studio: one persistent scene, nodes moved per frame ──────────────────
class Studio:
    def __init__(self):
        self.r = pyrender.OffscreenRenderer(W, H)
        self.sc = pyrender.Scene(bg_color=[0.035, 0.038, 0.045, 1.0], ambient_light=[0.10, 0.10, 0.11])
        self.meshes, self.nodes, self.screens = {}, {}, {}
        desk = trimesh.creation.box([1400, 1400, 2])
        self.sc.add(pyrender.Mesh.from_trimesh(desk, material=scene.material([0.16, 0.13, 0.11, 1.0], 0.8)),
                    pose=TR.translation_matrix([0, 0, -1.0]))
        self.cam = self.sc.add(pyrender.PerspectiveCamera(yfov=0.62, aspectRatio=W / H, znear=5, zfar=4000))
        self.key = self.sc.add(pyrender.DirectionalLight(color=[1.0, 0.97, 0.92], intensity=3.2))
        self.fill = self.sc.add(pyrender.DirectionalLight(color=[0.6, 0.7, 1.0], intensity=1.0),
                                pose=scene.look_at([-300, 250, 150], [0, 0, 80]))
        self.rim = self.sc.add(pyrender.DirectionalLight(color=[1.0, 0.6, 0.35], intensity=1.2),
                               pose=scene.look_at([-250, -50, 350], [0, 0, 80]))
        # One spot light for the whole film (a new light per frame would leak
        # its shadow map): moved with the head, switched off with intensity 0.
        self.spot_light = pyrender.SpotLight(color=[0.55, 0.7, 1.0], intensity=0.0,
                                             innerConeAngle=0.35, outerConeAngle=0.95)
        self.spot = self.sc.add(self.spot_light)
        self.cables = {}
        glass = trimesh.creation.cylinder(radius=18.6, height=8.0, sections=96)
        self.board = pyrender.Mesh.from_trimesh(glass, material=scene.material([0.02, 0.02, 0.025, 1], 0.25))
        self.cable_mesh = None

    def mesh(self, name):
        key = "knob" if name.startswith("knob") else name.split("#")[0]
        if key == "ball" and key not in self.meshes:   # ballast: steel balls
            self.meshes[key] = pyrender.Mesh.from_trimesh(trimesh.creation.icosphere(2, radius=4.2),
                                                          material=scene.material([0.7, 0.72, 0.75, 1], 0.25, 0.9))
        if key not in self.meshes:
            col = scene.COLORS.get(key, scene.GRAPHITE)
            self.meshes[key] = pyrender.Mesh.from_trimesh(scene.part_mesh(key),
                                                          material=scene.material(col, 0.35 if col is scene.ORANGE else 0.62),
                                                          smooth=False)
        return self.meshes[key]

    # Nodes are parked far below the desk rather than removed, and meshes are
    # created once: the scene itself stays constant. (The pyrender + PyOpenGL
    # 3.1.10 + OSMesa stack still leaks per render() call, 30–65 MB a frame,
    # shadows or not — hence the chunked rendering in cmd_render.)
    PARK = TR.translation_matrix([0, 0, -50000.0])

    def set(self, name, tf):
        """Show part `name` at `tf` (None parks it out of sight)."""
        if tf is None:
            if name in self.nodes:
                self.sc.set_pose(self.nodes[name], self.PARK)
            return
        if name in self.nodes:
            self.sc.set_pose(self.nodes[name], tf)
        else:
            self.nodes[name] = self.sc.add(self.mesh(name), pose=tf)

    def show_only(self, parts):
        for n in list(self.nodes):
            if n not in parts and not n.startswith("_"):
                self.set(n, None)
        for n, tf in parts.items():
            if tf is not None:
                self.set(n, tf)

    def screen(self, png, head_tf, lit=True, board_offset=0.0):
        """The display on the head (png) — or None to hide it."""
        if "_board" not in self.nodes:
            self.nodes["_board"] = self.sc.add(self.board, pose=self.PARK)
        for k in [k for k in self.nodes if k.startswith("_disc:")]:
            self.sc.set_pose(self.nodes[k], self.PARK)
        self.spot_light.intensity = 0.0
        if png is None:
            self.sc.set_pose(self.nodes["_board"], self.PARK)
            return
        key = "_disc:" + png
        if key not in self.nodes:
            self.nodes[key] = self.sc.add(scene.screen_disc(png), pose=self.PARK)
        base = head_tf @ TR.translation_matrix([0, 0, lamp.GLASS_Z + board_offset])
        self.sc.set_pose(self.nodes["_board"], base @ TR.translation_matrix([0, 0, 4.6]))
        self.sc.set_pose(self.nodes[key], base @ TR.translation_matrix([0, 0, 0.25]))
        if lit:
            self.spot_light.intensity = 30000.0
            self.sc.set_pose(self.spot, base @ TR.translation_matrix([0, 0, -2]))

    def cable(self, points):
        for k in [k for k in self.nodes if k.startswith("_cable:")]:
            self.sc.set_pose(self.nodes[k], self.PARK)
        if points is None:
            return
        key = "_cable:" + str(hash(tuple(np.round(np.asarray(points, float).ravel(), 2))))
        if key not in self.nodes:
            segs = []
            for a, b in zip(points[:-1], points[1:]):
                if np.linalg.norm(np.subtract(b, a)) < 0.5:
                    continue
                segs.append(trimesh.creation.cylinder(radius=1.9, segment=[a, b], sections=14))
                segs.append(trimesh.creation.icosphere(1, radius=1.9).apply_translation(b))
            mesh = pyrender.Mesh.from_trimesh(trimesh.util.concatenate(segs),
                                              material=scene.material([0.05, 0.05, 0.055, 1], 0.5), smooth=True)
            self.nodes[key] = self.sc.add(mesh, pose=self.PARK)
        self.sc.set_pose(self.nodes[key], np.eye(4))

    def render(self, eye, target):
        self.sc.set_pose(self.cam, scene.look_at(eye, target))
        self.sc.set_pose(self.key, scene.look_at(np.array(eye) + np.array([120, -260, 420]), target))
        c, _ = self.r.render(self.sc, flags=pyrender.RenderFlags.SHADOWS_DIRECTIONAL)
        return Image.fromarray(c)


# ── motion helpers ──────────────────────────────────────────────────────────
def ease(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def seg(t, a, b):
    """Progress of t through [a, b], eased."""
    return ease((t - a) / max(b - a, 1e-6))


def screw(final_tf, t, travel, pitch, axis_local=(0, 0, 1)):
    """Screw a part in along its local axis: `travel` mm out at t=0, turning at `pitch`."""
    d = (1 - t) * travel
    ang = -d / pitch * 2 * math.pi
    ax = np.array(axis_local, float)
    return final_tf @ TR.translation_matrix(-ax * d) @ TR.rotation_matrix(ang, ax)


def slide(final_tf, t, offset_world):
    m = final_tf.copy()
    m[:3, 3] = final_tf[:3, 3] + (1 - t) * np.array(offset_world, float)
    return m


def orbit(t, radius, height, a0, a1, target):
    a = math.radians(a0 + (a1 - a0) * t)
    return [target[0] + radius * math.cos(a), target[1] + radius * math.sin(a), height], target


def cable_points(P):
    """USB cable: out of the head's underside window, under the upper arm,
    along the lower arm, into the base channel and off the back of the desk."""
    pts = [P["head"] @ np.array([-24.0, 0, lamp.GLASS_Z + 6.5, 1.0]), P["head"] @ np.array([-34.0, 0, lamp.GLASS_Z + 8.5, 1.0])]
    pts = [p[:3] for p in pts]
    S, E, Hh = P["_points"]["S"], P["_points"]["E"], P["_points"]["H"]

    def along(a, b, k, off_v):
        u = (b - a) / np.linalg.norm(b - a)
        v = np.array([u[2], 0, -u[0]])      # in-plane perpendicular (arm_tf's local Y)
        return a + k * (b - a) + off_v * v + np.array([0, 8.0, 0])

    for k in (0.15, 0.5, 0.85):
        pts.append(along(Hh, E, k, -13.2))
    for k in (0.12, 0.5, 0.85):
        pts.append(along(E, S, k, 13.2))
    pts += [np.array([-8.0, 8.0, 34.0]), np.array([-18.0, 0, 20.5]), np.array([-40.0, 0, 18.4]),
            np.array([-56.0, 0, 16.0]), np.array([-120.0, 0, 1.9])]
    return pts


def clip_tfs(P):
    out = {}
    for i, (arm, k) in enumerate((("upper_arm", 0.5), ("lower_arm", 0.5), ("lower_arm", 0.85))):
        A = P[arm]
        u, v = A[:3, 0], A[:3, 1]
        y0 = A[1, 3]
        p = A[:3, 3] + u * (k * (lamp.L2 if arm == "upper_arm" else lamp.L1))
        R = np.eye(4)
        R[:3, 0] = [0, 1, 0]
        R[:3, 1] = -v if arm == "lower_arm" else v
        R[:3, 2] = np.cross(R[:3, 0], R[:3, 1])
        R[:3, 3] = [p[0], y0 + lamp.T / 2, p[2]]
        out[f"cable_clip#{i}"] = R @ TR.translation_matrix([0, 0, -3.0])
    return out


def layout_parts():
    """Every printable part laid out flat on the desk, as it comes off the printer."""
    spots = [("base", -150, -60), ("base_lid", -150, 70), ("head", -30, 70), ("shade", 55, 95),
             ("shoulder", 50, -10), ("jam_nut", 110, -10), ("lower_arm", -60, -95), ("upper_arm", -60, -140),
             ("knob#0", 110, 40), ("knob#1", 140, 40), ("knob#2", 170, 40), ("cable_clip#0", 150, -70),
             ("cable_clip#1", 170, -70), ("cable_clip#2", 190, -70), ("shim_1mm", 130, 110),
             ("shim_2mm", 175, 110), ("fit_coupon", -60, 165)]
    return {n: TR.translation_matrix([x, y, 0]) for n, x, y in spots}


# ── rendering the shots ─────────────────────────────────────────────────────
def durations():
    return json.loads((WORK / "durations.json").read_text())


def shot_frames(st, sid, dur):
    P = lamp.pose()
    parts = {n: tf for n, tf in P.items() if not n.startswith("_")}
    S, E, Hh = P["_points"]["S"], P["_points"]["E"], P["_points"]["H"]
    center = [25, 0, 95]
    n = int(round(dur * FPS))
    home = str(SIM / "04_home_running.png")
    for i in range(n):
        t = i / max(n - 1, 1)
        if (WORK / "3d" / sid / f"{i:04d}.png").exists() and os.environ.get("RESUME"):
            continue
        if sid == "intro":
            st.show_only(parts)
            st.screen(home, P["head"])
            st.cable(cable_points(P))
            for k, tf in clip_tfs(P).items():
                st.set(k, tf)
            eye, tgt = orbit(t, 340, 210 - 50 * t, -20, 25, center)
        elif sid == "parts":
            st.cable(None)
            st.screen(None, None)
            st.show_only(layout_parts())
            eye, tgt = orbit(t, 380, 330, -70, -40, [20, 0, 0])
        elif sid == "step1":
            # base turned over (as you would): ballast in the chambers, lid screwed on
            flip = TR.translation_matrix([0, 0, lamp.BASE_H]) @ TR.rotation_matrix(math.pi, [1, 0, 0])
            show = {"base": flip, "base_lid": screw(flip, seg(t, 0.45, 0.95), 55, lamp.LID_P * 4)}
            fill = seg(t, 0.0, 0.4)
            for k in range(8):
                if fill * 8 > k:
                    a = math.radians(45 * k + 22.5)
                    for j, r in enumerate((24.0, 33.0)):
                        show[f"ball#{k}_{j}"] = TR.translation_matrix([r * math.cos(a), r * math.sin(a), 8.0])
            st.show_only(show)
            eye, tgt = [200, -230, 190], [0, 0, 10]
        elif sid == "step2":
            show = {k: parts[k] for k in ("base", "base_lid")}
            show["shoulder"] = screw(parts["shoulder"], seg(t, 0.05, 0.55), 40, 8.0)
            # the nut rides down with the stud, then spins down onto the base to lock
            nut_ride = TR.translation_matrix([0, 0, (1 - seg(t, 0.05, 0.55)) * 40 + (1 - seg(t, 0.6, 0.9)) * 6])
            show["jam_nut"] = nut_ride @ parts["jam_nut"] @ TR.rotation_matrix(-seg(t, 0.6, 0.9) * 6, [0, 0, 1])
            st.show_only(show)
            eye, tgt = orbit(t, 260, 110, -40, -10, [0, 0, 40])
        elif sid == "step3":
            show = {k: parts[k] for k in ("base", "base_lid", "shoulder", "jam_nut")}
            show["lower_arm"] = slide(parts["lower_arm"], seg(t, 0.0, 0.45), [0, 70, 20])
            show["knob_shoulder"] = screw(parts["knob_shoulder"], seg(t, 0.45, 0.95), 40, 2.0 * 3)
            st.show_only(show)
            eye, tgt = [140, -260, 120], [-10, 0, 90]
        elif sid == "step4":
            show = {k: parts[k] for k in ("base", "base_lid", "shoulder", "jam_nut", "lower_arm", "knob_shoulder")}
            show["upper_arm"] = slide(parts["upper_arm"], seg(t, 0.0, 0.45), [0, -70, 30])
            show["knob_elbow"] = screw(parts["knob_elbow"], seg(t, 0.45, 0.95), 40, 6.0)
            st.show_only(show)
            eye, tgt = [150, 280, 190], [0, 0, 130]
        elif sid == "step5":
            # the head alone, on the desk, screen up toward the camera
            hd = TR.translation_matrix([0, 0, 52.4]) @ TR.rotation_matrix(math.pi, [1, 0, 0]) @ TR.rotation_matrix(math.pi / 2, [0, 0, 1])
            board_in = seg(t, 0.0, 0.4)
            seat = lamp.GLASS_Z + lamp.SEAT_Z - 1.0
            st.show_only({"head": hd, "shim_1mm": hd @ TR.translation_matrix([0, 0, seat - 30 * (1 - seg(t, 0.0, 0.2))]),
                          "shade": screw(hd @ TR.translation_matrix([0, 0, lamp.SHADE_MOUTH_Z]), seg(t, 0.45, 0.95), 30, 2.0 * 3)})
            st.screen(str(SIM / "02_offline.png"), hd, lit=False, board_offset=-(1 - board_in) * 45)
            eye, tgt = [110, -120, 170], [0, 0, 40]
        elif sid == "step6":
            show = {k: tf for k, tf in parts.items() if k not in ("head", "shade", "knob_head")}
            head_t = seg(t, 0.0, 0.35)
            hd = slide(P["head"], head_t, [0, -60, -10])
            show["head"] = hd
            show["shade"] = hd @ TR.translation_matrix([0, 0, lamp.SHADE_MOUTH_Z])
            show["knob_head"] = screw(parts["knob_head"], seg(t, 0.35, 0.6), 40, 6.0)
            clips = clip_tfs(P)
            for j, (k, tf) in enumerate(clips.items()):
                if t >= 0.66 + 0.08 * j:      # each clip arrives in turn
                    show[k] = slide(tf, seg(t, 0.66 + 0.08 * j, 0.78 + 0.08 * j), [0, 0, 60])
            st.show_only(show)
            st.screen(str(SIM / "02_offline.png"), hd, lit=False)
            st.cable(cable_points(P) if t > 0.62 else None)
            eye, tgt = orbit(t, 380, 170, 60, 20, center)
        elif sid in ("connect", "use", "outro"):
            st.show_only({**parts, **clip_tfs(P)})
            st.cable(cable_points(P))
            if sid == "connect":
                png = str(SIM / ("02_offline.png" if t < 0.45 else "03_home.png" if t < 0.7 else "04_home_running.png"))
                eye, tgt = orbit(t, 330, 150, 5, 15, center)
            elif sid == "use":
                seq = ["06_home_agent2.png", "08_question.png", "09_question_selected.png", "11_answered.png",
                       "05_home_stop_armed.png", "15_settings.png", "16_scrollpad.png"]
                png = str(SIM / seq[min(int(t * len(seq)), len(seq) - 1)])
                sc = P["_points"]["screen_center"]
                nm = P["_points"]["screen_normal"]
                eye = list(sc + nm * 150 + np.array([0, -40, 25]))
                tgt = list(sc)
            else:
                png = home
                eye, tgt = orbit(t, 380, 230, 55, 12, center)
            st.screen(png, P["head"])
        else:   # flash: 2D slide (composed later), keep a dim hero behind
            st.show_only({**parts, **clip_tfs(P)})
            st.cable(cable_points(P))
            st.screen(str(SIM / "01_boot.png"), P["head"])
            eye, tgt = [420, 170, 200], center
        img = st.render(eye, tgt)
        img.save(WORK / "3d" / sid / f"{i:04d}.png")
        # MAXFRAMES=N: stop after N frames with exit code 3 ("more to do");
        # cmd_render reruns with RESUME=1 until done. Unset = no limit.
        global _rendered
        _rendered += 1
        limit = int(os.environ.get("MAXFRAMES") or 0)
        if limit and _rendered >= limit:
            sys.exit(3)


_rendered = 0


def cmd_render_chunk(only=None):
    (WORK / "3d").mkdir(parents=True, exist_ok=True)
    d = durations()
    st = Studio()
    for s_ in SCENES:
        if only and s_["id"] not in only:
            continue
        (WORK / "3d" / s_["id"]).mkdir(parents=True, exist_ok=True)
        shot_frames(st, s_["id"], d[s_["id"]])
        print("rendered", s_["id"], flush=True)


def cmd_render(only=None):
    """Renders in short-lived processes of CHUNK frames (the GL stack leaks
    per frame), resuming where the previous one stopped."""
    env = dict(os.environ, RESUME="1", MAXFRAMES=os.environ.get("CHUNK", "30"))
    while True:
        rc = subprocess.run([sys.executable, __file__, "render-chunk", *(only or [])], env=env).returncode
        if rc == 0:
            return
        if rc != 3:
            sys.exit(f"render failed (exit {rc})")


# ── voice ───────────────────────────────────────────────────────────────────
VOICE = {"fr": "fr_FR-siwis-medium.onnx", "en": "en_US-lessac-medium.onnx"}


def wav_len(p):
    with wave.open(str(p)) as w:
        return w.getnframes() / w.getframerate()


def cmd_voice():
    WORK.mkdir(parents=True, exist_ok=True)
    d = {}
    for s in SCENES:
        longest = 0.0
        for lang in ("fr", "en"):
            out = WORK / "voice" / lang / f"{s['id']}.wav"
            out.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([sys.executable, "-m", "piper", "-m", str(VOICES / VOICE[lang]), "-f", str(out),
                            "--length-scale", "1.0" if lang == "fr" else "1.02", "--sentence-silence", "0.25"],
                           input=s["say"][lang].encode(), check=True, capture_output=True)
            longest = max(longest, wav_len(out))
        d[s["id"]] = round(max(s["min"], longest + 1.2), 2)
    (WORK / "durations.json").write_text(json.dumps(d, indent=1))
    print(d, "total", round(sum(d.values()), 1), "s")


# ── compose ─────────────────────────────────────────────────────────────────
def font(sz, bold=True):
    return ImageFont.truetype(FONT_B if bold else FONT_R, sz)


def overlay(img, s, lang, t):
    img = img.convert("RGB").resize((1280, 720), Image.LANCZOS)
    d = ImageDraw.Draw(img, "RGBA")
    fade = min(1.0, t * 3)
    a = int(255 * fade)
    # title chip
    title = s["title"][lang]
    if s["id"] == "flash":
        pass                                   # the slide below carries the title
    elif "step" in s:
        badge = ("ÉTAPE" if lang == "fr" else "STEP") + f" {s['step']}/6"
        d.rounded_rectangle([40, 36, 40 + 16 + d.textlength(badge, font=font(22)), 76], 10,
                            fill=(255, 107, 20, a))
        d.text((48, 42), badge, font=font(22), fill=(12, 12, 14, a))
        d.text((60 + d.textlength(badge, font=font(22)), 38), title, font=font(30), fill=(235, 238, 242, a))
    else:
        d.text((40, 34), title, font=font(34), fill=(235, 238, 242, a))
        d.rectangle([40, 82, 110, 86], fill=(255, 107, 20, a))
    lines = CAPTION.get(s["id"], {}).get(lang)
    if s["id"] == "flash":
        panel = Image.new("RGBA", img.size, (8, 9, 12, 215))
        img.paste(panel, (0, 0), panel)
        d = ImageDraw.Draw(img, "RGBA")
        d.text((80, 90), s["title"][lang], font=font(46), fill=(240, 242, 245))
        d.rectangle([80, 150, 170, 155], fill=(255, 107, 20))
        y = 200
        for k, ln in enumerate(lines):
            d.ellipse([80, y + 6, 104, y + 30], fill=(255, 107, 20))
            d.text((86, y + 5), str(k + 1), font=font(18), fill=(10, 10, 12))
            d.text((122, y), ln, font=font(28, k != 0) if k else font(30), fill=(225, 230, 236))
            y += 70
        boot = Image.open(SIM / "02_offline.png").resize((250, 250))   # what it shows once flashed
        m = Image.new("L", boot.size, 0)
        ImageDraw.Draw(m).ellipse([0, 0, 249, 249], fill=255)
        img.paste(boot, (960, 380), m)
        return img
    if lines:
        box_h = 20 + 40 * len(lines)
        d.rounded_rectangle([40, 720 - 40 - box_h, 1240, 680], 16, fill=(8, 9, 12, int(190 * fade)))
        y = 720 - 40 - box_h + 12
        for k, ln in enumerate(lines):
            d.text((62, y), ln, font=font(26, k == 0), fill=(235, 238, 242, a))
            y += 40
    return img


def cmd_compose(langs=("fr", "en")):
    d = durations()
    for lang in langs:
        out = WORK / f"comp_{lang}"
        out.mkdir(parents=True, exist_ok=True)
        k = 0
        audio = []
        rate = None
        for s in SCENES:
            frames = sorted((WORK / "3d" / s["id"]).glob("*.png"))
            n = int(round(d[s["id"]] * FPS))
            for i in range(n):
                src = frames[min(i, len(frames) - 1)]
                overlay(Image.open(src), s, lang, i / FPS).save(out / f"{k:05d}.png")
                k += 1
            with wave.open(str(WORK / "voice" / lang / f"{s['id']}.wav")) as w:
                rate = w.getframerate()
                pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
            clip = np.zeros(int(n / FPS * rate), dtype=np.int16)
            lead = int(0.45 * rate)
            m = min(len(pcm), len(clip) - lead)
            clip[lead:lead + m] = pcm[:m]
            audio.append(clip)
        with wave.open(str(WORK / f"voice_{lang}.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(np.concatenate(audio).tobytes())
        mp4 = HERE / f"harness-c3-lamp-{lang}.mp4"
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(FPS), "-i", str(out / "%05d.png"),
                        "-i", str(WORK / f"voice_{lang}.wav"), "-vf", "fps=30,format=yuv420p",
                        "-c:v", "libx264", "-preset", "medium", "-crf", "23", "-c:a", "aac", "-b:a", "96k",
                        "-shortest", "-movflags", "+faststart", str(mp4)], check=True)
        print("wrote", mp4, round(mp4.stat().st_size / 1e6, 1), "MB")


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("voice", "all"):
        cmd_voice()
    if what == "render-chunk":
        cmd_render_chunk(sys.argv[2:] or None)
    if what in ("render", "all"):
        cmd_render(sys.argv[2:] or None)
    if what in ("compose", "all"):
        cmd_compose()
