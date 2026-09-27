#!/usr/bin/env python3
"""Harness C3 desk lamp — parametric, print-ready STL generator.

An original two-arm articulated desk lamp whose "bulb" is the round
ESP32-2424S012C touch display. Every part prints flat, without supports:
threads have 45° flanks, holes whose axis ends up horizontal are teardrops,
the only ceilings are short bridges between ribs.

    pip install manifold3d trimesh numpy
    python3 lamp.py              # writes stl/*.stl + stl/assembly_preview.stl

Units: millimetres. Joints: printed 10 mm × 2 mm knob screws clamp two 8 mm
plates face to face (friction joints: turn the knob to lock, loosen to move).
"""
import math
import pathlib
import sys

import numpy as np
import trimesh
from manifold3d import CrossSection, FillRule, JoinType, Manifold, OpType

OUT = pathlib.Path(__file__).resolve().parent / "stl"

# ── parameters ──────────────────────────────────────────────────────────────
T = 8.0            # arm plate thickness
HUB_R = 10.5       # joint hub radius
BAR_W = 12.0       # arm bar width
L1, L2 = 115.0, 105.0  # lower / upper arm, pivot to pivot

BOLT_D, BOLT_P = 10.0, 2.0   # knob screw thread
FIT = 0.30                   # radial play of every printed thread (FDM, 0.4 nozzle)
PASS_D = 10.8                # clearance hole for the knob screw

BOARD_D = 39.4     # ESP32-2424S012C is 38.5 × 37 mm: cavity with 0.45 mm play
SEAT_Z = 9.5       # glass front to the back rest (shims take up the rest)
HEAD_THREAD_D = 48.0

BASE_D, BASE_H = 100.0, 20.0
LID_THREAD_D, LID_P, LID_H = 86.0, 3.0, 6.0
STUD_D, STUD_P = 16.0, 2.0

SEG = 96           # circle resolution


# ── primitives ──────────────────────────────────────────────────────────────
def circle(r, n=SEG):
    return CrossSection.circle(r, n)


def poly(pts):
    return CrossSection([[tuple(map(float, q)) for q in pts]], FillRule.NonZero)


def revolve(profile_rz, n=SEG):
    """profile: [(r, z), …] closed polygon in the r/z half-plane."""
    return Manifold.revolve(poly(profile_rz), n)


def cyl(r, h, z0=0.0, n=SEG):
    return Manifold.cylinder(h, r, r, n).translate([0, 0, z0])


def thread_rod(d_major, pitch, length, grow=0.0, n=96, flat=0.2):
    """Single-start right-hand thread with 45° flanks: a twisted extrusion of
    r(θ) = r_minor + depth·tri(θ/2π). `grow` enlarges it radially (the cutter
    for a nut)."""
    depth = 0.5 * pitch * (1.0 - flat)
    r_minor = d_major / 2.0 - depth + grow
    pts = []
    for i in range(n):
        u = i / n
        tri = 1.0 - abs(2.0 * u - 1.0)
        tri = min(max(tri, flat / 2), 1 - flat / 2)
        tri = (tri - flat / 2) / (1 - flat)
        r = r_minor + depth * tri
        th = 2 * math.pi * u
        pts.append((r * math.cos(th), r * math.sin(th)))
    div = max(8, int(length / pitch * 24))
    return Manifold.extrude(poly(pts), length, div, 360.0 * length / pitch)


def male_thread(d, p, length, chamfer=True):
    rod = thread_rod(d, p, length)
    if chamfer:   # 45° lead-in at the tip (z = 0) so it starts cleanly
        r = d / 2
        cone = revolve([(0, 0), (r - 1.0, 0), (r + 0.5, 1.5), (r + 0.5, length), (0, length)])
        rod = rod ^ cone
    return rod


def female_cutter(d, p, length, z0=0.0, countersink=True):
    cut = thread_rod(d, p, length + 0.02, grow=FIT).translate([0, 0, z0 - 0.01])
    if countersink:   # chamfered entries at both ends
        r = d / 2 + FIT
        cut = cut + revolve([(0, z0 - 0.5), (r + 1.0, z0 - 0.5), (r - 0.6, z0 + 1.1), (0, z0 + 1.1)])
        z1 = z0 + length
        cut = cut + revolve([(0, z1 - 1.1), (r - 0.6, z1 - 1.1), (r + 1.0, z1 + 0.5), (0, z1 + 0.5)])
    return cut


def knurled_disc(r, h, notches, notch_r, chamfer=1.2):
    cs = circle(r, 160)
    cut = [circle(notch_r, 24).translate((r * math.cos(2 * math.pi * k / notches),
                                         r * math.sin(2 * math.pi * k / notches)))
           for k in range(notches)]
    cs = cs - CrossSection.batch_boolean(cut, OpType.Add)
    body = Manifold.extrude(cs, h)
    trim = revolve([(0, 0), (r - chamfer, 0), (r + 0.01, chamfer), (r + 0.01, h - chamfer),
                    (r - chamfer, h), (0, h)])
    return body ^ trim


def teardrop(r, n=48):
    """Hole profile in the X/Z plane, point up (+Z): prints without support."""
    c = circle(r, n)
    k = r / math.cos(math.radians(45))
    tip = poly([(-r * math.sin(math.radians(45)), r * math.cos(math.radians(45))),
                (0.0, k), (r * math.sin(math.radians(45)), r * math.cos(math.radians(45))),
                (0, 0)])
    return c + tip


def xz_plate(cs_xz, y0, y1):
    """Extrude a profile drawn in (X, Z) along +Y from y0 to y1."""
    m = Manifold.extrude(cs_xz, y1 - y0)          # profile in XY, extruded along Z
    # (x, y, z) → (x, z, y): rotate about X by +90 then fix orientation
    m = m.rotate([90, 0, 0])                       # (x, y, z) → (x, -z, y)
    return m.translate([0, y1, 0])


# ── parts ───────────────────────────────────────────────────────────────────
def knob():
    """Knob screw: knurled head (z 0..9) + 16 mm of 10×2 thread."""
    head = knurled_disc(12.0, 9.0, 14, 1.6)
    head = head - revolve([(0, 8.2), (2.5, 9.01), (0, 9.01)])      # tiny dimple, looks finished
    shank = male_thread(BOLT_D, BOLT_P, 16.0).rotate([180, 0, 0]).translate([0, 0, 9.0 + 16.0])
    return head + shank.translate([0, 0, 0])


def arm_plate(length, hole_a, hole_b):
    """Dog-bone arm printed flat. hole_*: 'thread' | 'pass'."""
    cs = circle(HUB_R) + circle(HUB_R).translate((length, 0)) + \
        CrossSection.square((length, BAR_W)).translate((0, -BAR_W / 2))
    cs = cs.offset(2.5, JoinType.Round).offset(-2.5, JoinType.Round)      # fillet the waist
    slot = CrossSection.square((length - 2 * 27, 4.4)).translate((27, -2.2))
    cs = cs - slot.offset(0.6, JoinType.Round)
    for x, kind in ((0.0, hole_a), (length, hole_b)):
        if kind == "pass":
            cs = cs - circle(PASS_D / 2).translate((x, 0))
    m = Manifold.extrude(cs, T)
    for x, kind in ((0.0, hole_a), (length, hole_b)):
        if kind == "thread":
            m = m - female_cutter(BOLT_D, BOLT_P, T).translate([x, 0, 0])
    return m


def base():
    r = BASE_D / 2
    prof = [(0, 0), (r, 0), (r, BASE_H - 4.0)]
    prof += [(r - 4.0 + 4.0 * math.cos(math.radians(a)), BASE_H - 4.0 + 4.0 * math.sin(math.radians(a)))
             for a in range(0, 91, 10)]
    prof += [(0, BASE_H)]
    b = revolve(prof, 160)
    # ballast chamber: annulus above the lid, 8 ribs keep every bridge short
    cav = revolve([(17.0, LID_H - 0.01), (40.5, LID_H - 0.01), (40.5, 14.0), (17.0, 14.0)], 160)
    ribs = [Manifold.cube([26.0, 2.4, 9.0]).translate([15.5, -1.2, LID_H - 0.5]).rotate([0, 0, 45 * k])
            for k in range(8)]
    b = b - (cav - Manifold.batch_boolean(ribs, OpType.Add))
    # threaded bottom opening for the lid
    b = b - female_cutter(LID_THREAD_D, LID_P, LID_H, countersink=False)
    b = b - revolve([(0, -0.5), (LID_THREAD_D / 2 + FIT + 1.2, -0.5), (LID_THREAD_D / 2 + FIT - 0.4, 1.1),
                     (0, 1.1)], 160)
    # centre socket for the shoulder stud (12 deep)
    b = b - female_cutter(STUD_D, STUD_P, 12.0, z0=BASE_H - 12.0)
    # cable channel on top, out the back
    ch = Manifold.cylinder(40.0, 2.6, 2.6, 32).rotate([0, -90, 0]).translate([-16.0, 0, BASE_H - 0.6])
    b = b - ch - Manifold.cube([40.0, 5.2, 3.0]).translate([-56.0, -2.6, BASE_H - 0.6])
    return b


def base_lid():
    lid = male_thread(LID_THREAD_D, LID_P, LID_H, chamfer=False)
    r = LID_THREAD_D / 2
    lid = lid ^ revolve([(0, 0), (r - 0.9, 0), (r + 0.5, 1.4), (r + 0.5, LID_H - 0.6),
                         (r - 1.1, LID_H), (0, LID_H)], 160)
    for k in range(3):                      # fingertip / spanner holes
        a = math.radians(120 * k + 30)
        lid = lid - cyl(3.2, 3.2, -0.01).translate([28 * math.cos(a), 28 * math.sin(a), 0])
    for k in range(4):                      # rubber feet recesses (Ø10 × 1)
        a = math.radians(90 * k)
        lid = lid - cyl(5.2, 1.0, -0.01).translate([36 * math.cos(a), 36 * math.sin(a), 0])
    return lid


SHOULDER_PIVOT_Z = 44.0   # above the stud tip (part frame)


def shoulder():
    """Stud (M16×2) + 45° collar + fin; printed stud-down."""
    stud = male_thread(STUD_D, STUD_P, 16.0)
    collar = revolve([(0, 16.0), (STUD_D / 2, 16.0), (12.5, 20.5), (0, 20.5)])
    fin_cs = poly([(-12.0, 20.0), (12.0, 20.0), (9.0, SHOULDER_PIVOT_Z), (-9.0, SHOULDER_PIVOT_Z)]) + \
        circle(HUB_R).translate((0, SHOULDER_PIVOT_Z))
    fin_cs = fin_cs.offset(2.0, JoinType.Round).offset(-2.0, JoinType.Round)
    fin_cs = fin_cs - teardrop(PASS_D / 2).translate((0, SHOULDER_PIVOT_Z))
    fin = xz_plate(fin_cs, -T / 2, T / 2)
    return stud + collar + fin


def jam_nut():
    n = knurled_disc(15.0, 6.0, 16, 1.8, chamfer=1.0)
    return n - female_cutter(STUD_D, STUD_P, 6.0)


# head: local frame, axis +Z, front (screen) at z = 0, "up" = +X
HEAD_PIVOT = (31.0, 30.0)   # (x, z) of the joint hub in head coordinates


def head():
    lip = male_thread(HEAD_THREAD_D, 2.0, 7.0, chamfer=False)
    r = HEAD_THREAD_D / 2
    lip = lip ^ revolve([(0, 0), (r - 1.0, 0), (r + 0.5, 1.5), (r + 0.5, 7.0), (0, 7.0)])
    body = revolve([(0, 7.0), (26.0, 7.0), (26.0, 10.0), (16.0, 38.0), (12.0, 43.0), (6.0, 45.6), (0, 46.2)], 128)
    h = lip + body
    # joint tab: plate y ∈ [-12, -4], slanted underside (≥ 45°) toward the body
    px, pz = HEAD_PIVOT
    tab_cs = CrossSection.batch_hull([circle(HUB_R).translate((px, pz)),
                                      poly([(12.0, 10.0), (23.0, 10.0), (20.0, 40.0), (10.0, 40.0)])])
    tab_cs = tab_cs - teardrop(PASS_D / 2).translate((px, pz))
    h = h + xz_plate(tab_cs, -12.0, -4.0)
    # board cavity: Ø39.4 to the seat, a 2 mm rest, then a 45° cone (no bridge)
    rb = BOARD_D / 2
    h = h - revolve([(0, -1.0), (rb, -1.0), (rb, SEAT_Z), (rb - 2.2, SEAT_Z), (rb - 2.2, SEAT_Z + 1.5),
                     (0, SEAT_Z + 1.5 + (rb - 2.2))], 128)
    # USB-C window on the underside (-X): the plug enters radially
    h = h - Manifold.cube([16.0, 15.0, 11.0]).translate([-rb - 12.0, -7.5, 1.5])
    # vent / light slots on the back cone, purely cosmetic
    for k in range(6):
        a = 60 * k + 30
        slot = Manifold.cube([2.0, 6.0, 9.0], True).translate([19.0, 0, 26.0]).rotate([0, 0, a])
        h = h - slot
    return h


def bezel():
    """Screws onto the head's front thread; its lip holds the glass rim."""
    outer = knurled_disc(27.8, 8.6, 36, 1.0, chamfer=0.8)
    ring = outer - female_cutter(HEAD_THREAD_D, 2.0, 7.2, z0=1.4, countersink=False)
    ring = ring - cyl(34.4 / 2, 2.0, -0.5)                      # window onto the display
    ring = ring - revolve([(0, -0.01), (34.4 / 2 + 1.2, -0.01), (34.4 / 2, 1.2), (0, 1.2)])  # soft inner edge
    return ring


def shim(t):
    ring = cyl(19.3, t) - cyl(15.0, t + 1, -0.5)
    return ring - Manifold.cube([16.0, 40.0, t + 2], True).translate([-19.0, 0, 0])   # USB-C side open


def cable_clip():
    """Snaps over an arm (12 × 8), holds a USB cable (Ø3.5–4.5)."""
    outer = CrossSection.square((12.4, 16.4), True).offset(1.6, JoinType.Round)
    inner = CrossSection.square((8.3, 12.4), True)
    mouth = CrossSection.square((6.0, 8.0), True).translate((0, 8.0))
    ring = circle(4.3, 48).translate((0, -13.2)) - circle(2.35, 48).translate((0, -13.2)) - \
        CrossSection.square((1.8, 4), True).translate((0, -17.0))
    cs = outer - inner - mouth + ring
    return Manifold.extrude(cs, 6.0)


def fit_coupon():
    """The first 12 mm of the head: test the board fit and the bezel thread."""
    return head() ^ Manifold.cube([200, 200, 12.0]).translate([-100, -100, 0])


# ── assembly (for previews / renders) ───────────────────────────────────────
def pose(shoulder_deg=102.0, elbow_deg=-18.0, head_down_deg=14.0):
    """World transforms of every part: X forward, Z up, Y to the lamp's left."""
    S = np.array([0.0, 0.0, 10.0 + SHOULDER_PIVOT_Z])
    a1 = math.radians(shoulder_deg)
    E = S + L1 * np.array([math.cos(a1), 0, math.sin(a1)])
    a2 = math.radians(elbow_deg)
    H = E + L2 * np.array([math.cos(a2), 0, math.sin(a2)])

    def arm_tf(p0, ang, y0):
        c, s = math.cos(ang), math.sin(ang)
        R = np.eye(4)
        R[:3, 0] = [c, 0, s]
        R[:3, 1] = [s, 0, -c]
        R[:3, 2] = [0, 1, 0]
        R[:3, 3] = p0 + np.array([0, y0, 0])
        return R

    def knob_tf(p, toward_plus_y, face_y):
        R = np.eye(4)
        if toward_plus_y:    # local +Z → world +Y
            R[:3, :3] = [[1, 0, 0], [0, 0, 1], [0, -1, 0]]
            R[:3, 3] = [p[0], face_y - 9.0, p[2]]
        else:                # local +Z → world −Y
            R[:3, :3] = [[1, 0, 0], [0, 0, -1], [0, 1, 0]]
            R[:3, 3] = [p[0], face_y + 9.0, p[2]]
        return R

    b = math.radians(head_down_deg)
    phi = math.atan2(-math.cos(b), math.sin(b))          # screen normal = (cos b, 0, −sin b)
    Rh = np.eye(4)
    Rh[:3, :3] = [[math.cos(phi), 0, math.sin(phi)], [0, 1, 0], [-math.sin(phi), 0, math.cos(phi)]]
    piv_local = np.array([HEAD_PIVOT[0], 0, HEAD_PIVOT[1], 1.0])
    Rh[:3, 3] = H - (Rh @ piv_local)[:3]
    screen_center = (Rh @ np.array([0, 0, -0.2, 1.0]))[:3]
    screen_normal = Rh[:3, :3] @ np.array([0, 0, -1.0])

    tr = trimesh.transformations.translation_matrix
    return {
        "base": np.eye(4), "base_lid": np.eye(4),
        "shoulder": tr([0, 0, 10.0]), "jam_nut": tr([0, 0, BASE_H]),
        "lower_arm": arm_tf(S, a1, 4.0), "upper_arm": arm_tf(E, a2, -4.0),
        "knob_shoulder": knob_tf(S, True, -4.0), "knob_elbow": knob_tf(E, False, 12.0),
        "knob_head": knob_tf(H, True, -12.0),
        "head": Rh, "bezel": Rh @ tr([0, 0, -1.4]),
        "_points": {"S": S, "E": E, "H": H, "screen_center": screen_center, "screen_normal": screen_normal},
    }


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3],
                           faces=np.asarray(mesh.tri_verts), process=False)


PARTS = {
    # name: (builder, quantity, what for)
    "base": (base, 1, "weighted foot, ballast chamber + cable channel"),
    "base_lid": (base_lid, 1, "screws into the base, closes the ballast"),
    "shoulder": (shoulder, 1, "M16 stud + fin: first joint, swivels on the base"),
    "jam_nut": (jam_nut, 1, "locks the shoulder's direction"),
    "lower_arm": (lambda: arm_plate(L1, "thread", "pass"), 1, "arm 1"),
    "upper_arm": (lambda: arm_plate(L2, "thread", "thread"), 1, "arm 2"),
    "knob": (knob, 3, "friction joint screws (shoulder, elbow, head)"),
    "head": (head, 1, "lamp head: holds the display as its bulb"),
    "bezel": (bezel, 1, "screws onto the head, holds the glass"),
    "shim_1mm": (lambda: shim(1.0), 1, "board depth spacers (use 0–2)"),
    "shim_2mm": (lambda: shim(2.0), 1, ""),
    "cable_clip": (cable_clip, 3, "hold the USB cable along the arms"),
    "fit_coupon": (fit_coupon, 1, "print first: board fit + bezel thread check"),
}


def main():
    OUT.mkdir(exist_ok=True)
    built = {}
    for name, (fn, qty, _) in PARTS.items():
        m = fn()
        st = m.status()
        if str(st) != "Error.NoError" and "NoError" not in str(st):
            sys.exit(f"{name}: manifold error {st}")
        tm = to_trimesh(m)
        assert tm.is_watertight, name
        tm.export(OUT / f"{name}.stl")
        built[name] = tm
        ext = tm.bounds[1] - tm.bounds[0]
        print(f"{name:12s} ×{qty}  {ext[0]:6.1f} × {ext[1]:6.1f} × {ext[2]:5.1f} mm  "
              f"{tm.volume / 1000:6.1f} cm³  watertight")
    P = pose()
    scene = []
    for name, tf in P.items():
        if name.startswith("_"):
            continue
        key = "knob" if name.startswith("knob") else name
        mesh = built[key].copy()
        mesh.apply_transform(tf)
        scene.append(mesh)
    asm = trimesh.util.concatenate(scene)
    asm.export(OUT / "assembly_preview.stl")
    ext = asm.bounds[1] - asm.bounds[0]
    print(f"assembled: {ext[0]:.0f} × {ext[1]:.0f} × {ext[2]:.0f} mm; pivots {P['_points']}")


if __name__ == "__main__":
    main()
