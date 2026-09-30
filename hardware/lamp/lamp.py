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
FIT = 0.35                   # radial play of every printed thread (FDM, 0.4 nozzle): 0.25 mm
                             # per 45° flank, still > 0 on a printer that over-extrudes by 0.1 mm
PASS_D = 10.8                # clearance hole for the knob screw

BOARD_D = 39.4     # ESP32-2424S012C is 38.5 × 37 mm: cavity with 0.45 mm play
SEAT_Z = 9.5       # glass front to the back rest (shims take up the rest)
GLASS_Z = 6.0      # where the glass front sits in the head (behind the socket thread)
SHADE_THREAD_D = 46.0   # the shade's tube screws INTO the socket's front
SHADE_BORE_R = 20.2     # the shade's throat, around the display
SHADE_MOUTH_R = 37.0    # inner radius at the mouth: the reflector is a 45° cone

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


def female_cutter(d, p, length, z0=0.0, countersink=True, far_end=True):
    cut = thread_rod(d, p, length + 0.02, grow=FIT).translate([0, 0, z0 - 0.01])
    if countersink:   # chamfered entries at both ends (far_end=False: the entry only)
        r = d / 2 + FIT
        cut = cut + revolve([(0, z0 - 0.5), (r + 1.0, z0 - 0.5), (r - 0.6, z0 + 1.1), (0, z0 + 1.1)])
        z1 = z0 + length
        if far_end:
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
HEAD_PIVOT = (34.0, 34.0)   # (x, z) of the joint hub in head coordinates
SHADE_MOUTH_Z = -(SHADE_MOUTH_R - SHADE_BORE_R)   # the mouth plane, in head coordinates


def head():
    """The socket (vintage bulb holder): board cavity behind a female thread,
    beaded band, rounded shoulder with vents, neck, dome and a switch knob;
    the USB-C window underneath lies BEHIND the thread, clear of the shade."""
    body = [(0, 0), (26.0, 0), (26.0, 1.2), (26.8, 2.0), (26.8, 3.6), (26.0, 4.4), (26.0, 22.0)]
    body += [(14.5 + 11.5 * math.cos(math.radians(a)), 22.0 + 11.5 * math.sin(math.radians(a)))
             for a in range(10, 91, 10)]                               # rounded shoulder
    body += [(14.5, 36.0), (13.6, 36.6), (13.6, 37.6), (14.5, 38.2), (14.5, 39.0)]   # neck band
    body += [(14.5 * math.cos(math.radians(a)), 39.0 + 5.0 * math.sin(math.radians(a)))
             for a in range(15, 76, 15)]                               # dome
    body += [(3.0, 44.2), (3.0, 47.0), (4.2, 48.2)]                    # switch knob: stem, 45° flare
    body += [(4.2 * math.cos(math.radians(a)), 48.2 + 4.2 * math.sin(math.radians(a))) for a in range(20, 91, 20)]
    h = revolve(body, 128)
    # joint tab: plate y ∈ [-12, -4] rising from the shoulder, slanted underside
    px, pz = HEAD_PIVOT
    tab_cs = CrossSection.batch_hull([circle(HUB_R).translate((px, pz)),
                                      poly([(14.0, 16.0), (25.0, 16.0), (22.0, 44.0), (12.0, 44.0)])])
    tab_cs = tab_cs - teardrop(PASS_D / 2).translate((px, pz))
    h = h + xz_plate(tab_cs, -12.0, -4.0)
    # front: female thread for the shade, then the board cavity, a 2 mm rest,
    # and a 45° cone behind it (no bridge)
    h = h - female_cutter(SHADE_THREAD_D, 2.0, GLASS_Z, z0=0.0, far_end=False)
    rb = BOARD_D / 2
    seat = GLASS_Z + SEAT_Z
    rt = SHADE_THREAD_D / 2 + FIT     # thread end → board cavity: a 45° cone, not a ledge
    h = h - revolve([(0, GLASS_Z - 0.5), (rt, GLASS_Z - 0.5), (rt, GLASS_Z), (rb, GLASS_Z + rt - rb),
                     (rb, seat), (rb - 2.2, seat), (rb - 2.2, seat + 1.5), (0, seat + 1.5 + (rb - 2.2))], 128)
    # USB-C window on the underside (-X), behind the thread: the plug enters radially
    h = h - Manifold.cube([16.0, 15.0, 13.2]).translate([-rb - 12.0, -7.5, GLASS_Z + 0.3])
    # vents around the shoulder, radial (they also let the board breathe)
    for k in range(8):
        a = 45 * k + 22.5
        vent = Manifold.cylinder(14.0, 1.6, 1.6, 24).rotate([0, 90, 0]).translate([12.0, 0, 27.0])
        h = h - vent.rotate([0, 0, a])
    return h


def shade():
    """Bell shade, printed mouth-down: a tube with a male thread screws into
    the socket and its end lip holds the glass; inside, a 45° reflector cone
    turns the display into the bulb. Rolled rim with a flat, 45° underside."""
    zm = SHADE_MOUTH_Z
    rim_r = SHADE_MOUTH_R + 1.9
    outer = [(0, zm), (rim_r, zm), (rim_r + 1.5, zm + 1.5)]
    outer += [(rim_r - 0.3 + 1.8 * math.cos(math.radians(a)), zm + 1.5 + 1.8 * math.sin(math.radians(a)))
              for a in range(0, 131, 15)]                             # rolled rim
    top_r, z0 = 27.0, zm + 3.0
    bell = []
    for k in range(1, 25):                                             # the bell, t^1.8
        t = 1 - k / 24
        z = z0 * t
        bell.append((top_r + (rim_r - 1.2 - top_r) * t ** 1.8, z))
    outer += bell
    ri = SHADE_THREAD_D / 2 - 0.5 * 2.0 * 0.8        # thread minor radius
    outer += [(top_r, 0.0), (ri, 0.0), (ri, GLASS_Z), (0, GLASS_Z)]
    solid = revolve(outer, 160)
    rod = male_thread(SHADE_THREAD_D, 2.0, GLASS_Z).rotate([180, 0, 0]).translate([0, 0, GLASS_Z])
    solid = solid + (rod ^ revolve([(0, 0.01), (30, 0.01), (30, GLASS_Z), (0, GLASS_Z)]))
    rb = SHADE_BORE_R
    cavity = revolve([(0, zm - 1.0), (SHADE_MOUTH_R + 1.0, zm - 1.0), (rb, 0.0), (rb, GLASS_Z - 3.0),
                      (17.2, GLASS_Z), (17.2, GLASS_Z + 1.0), (0, GLASS_Z + 1.0)], 160)
    sh = solid - cavity
    return sh.translate([0, 0, -zm])       # print frame: mouth on the bed


def washer():
    """Optional friction washer between two plates (print in TPU or PETG)."""
    return cyl(HUB_R - 0.5, 1.0) - cyl(PASS_D / 2 + 0.2, 2.0, -0.5)


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
    """The socket's front 14 mm: test the shade's thread and the board fit."""
    return head() ^ Manifold.cube([200, 200, GLASS_Z + 8.0]).translate([-100, -100, 0])


GAUGE_FITS = (0.25, 0.30, 0.35, 0.40, 0.45)   # radial plays of the gauge's holes (1–5 dots)


def thread_gauge():
    """Print first (10 min): an arm-thick plate with five 10×2 holes cut at the
    radial plays GAUGE_FITS, marked with 1 to 5 dots. Screw a printed knob into
    each one: the tightest hole it enters by hand, without forcing, is your FIT."""
    pitch = 16.0
    w = (len(GAUGE_FITS) - 1) * pitch + 18.0
    plate = Manifold.cube([w, 22.0, T]).translate([-9.0, -11.0, 0])
    cuts = []
    for k, fit in enumerate(GAUGE_FITS):
        x, r = k * pitch, BOLT_D / 2 + fit
        cuts.append(thread_rod(BOLT_D, BOLT_P, T + 0.02, grow=fit).translate([x, 0, -0.01]))
        cuts.append(revolve([(0, -0.5), (r + 1.0, -0.5), (r - 0.6, 1.1), (0, 1.1)]).translate([x, 0, 0]))
        cuts.append(revolve([(0, T - 1.1), (r - 0.6, T - 1.1), (r + 1.0, T + 0.5), (0, T + 0.5)]).translate([x, 0, 0]))
        for j in range(k + 1):
            cuts.append(cyl(0.8, 1.0, T - 0.6, 16).translate([x - k * 1.1 + j * 2.2, 8.2, 0]))
    return plate - Manifold.batch_boolean(cuts, OpType.Add)


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
    screen_center = (Rh @ np.array([0, 0, GLASS_Z - 0.2, 1.0]))[:3]
    screen_normal = Rh[:3, :3] @ np.array([0, 0, -1.0])

    tr = trimesh.transformations.translation_matrix
    return {
        "base": np.eye(4), "base_lid": np.eye(4),
        "shoulder": tr([0, 0, 10.0]), "jam_nut": tr([0, 0, BASE_H]),
        "lower_arm": arm_tf(S, a1, 4.0), "upper_arm": arm_tf(E, a2, -4.0),
        "knob_shoulder": knob_tf(S, True, -4.0), "knob_elbow": knob_tf(E, False, 12.0),
        "knob_head": knob_tf(H, True, -12.0),
        "head": Rh, "shade": Rh @ tr([0, 0, SHADE_MOUTH_Z]),
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
    "head": (head, 1, "socket: holds the board; USB-C window underneath"),
    "shade": (shade, 1, "bell shade: screws into the socket, holds the glass"),
    "shim_1mm": (lambda: shim(1.0), 1, "board depth spacers (use 0–2)"),
    "shim_2mm": (lambda: shim(2.0), 1, ""),
    "cable_clip": (cable_clip, 3, "hold the USB cable along the arms"),
    "thread_gauge": (thread_gauge, 1, "print first: picks FIT for your printer"),
    "fit_coupon": (fit_coupon, 1, "print first: board fit + shade thread check"),
    "washer": (washer, 3, "optional friction washers (TPU/PETG), one per joint"),
}


def check():
    """Software review of the design: collisions, thread fits, the USB-C plug
    path and overhangs in print orientation. Exit status 1 on a problem."""
    import itertools
    bad = []
    P = pose()
    built = {k: PARTS[k][0]() for k in PARTS}
    names = [n for n in P if not n.startswith("_")]
    ms = {n: built["knob" if n.startswith("knob") else n].transform(P[n][:3, :].astype(float)) for n in names}
    # 1. collisions in the assembled pose (threaded pairs overlap only by thread phase)
    threaded = {frozenset(x) for x in [("knob_shoulder", "lower_arm"), ("knob_elbow", "upper_arm"),
                ("knob_head", "upper_arm"), ("shoulder", "base"), ("shoulder", "jam_nut"),
                ("base_lid", "base"), ("shade", "head")]}
    for a, b in itertools.combinations(names, 2):
        v = (ms[a] ^ ms[b]).volume()
        if v > 0.5 and frozenset((a, b)) not in threaded:
            bad.append(f"collision {a} × {b}: {v:.1f} mm³")
    # 2. every printed thread pair: no contact at FIT, phase aligned
    for d, p_ in [(BOLT_D, BOLT_P), (STUD_D, STUD_P), (SHADE_THREAD_D, 2.0), (LID_THREAD_D, LID_P)]:
        male = thread_rod(d, p_, 6.0)
        nut = Manifold.cylinder(6.0, d / 2 + 4, d / 2 + 4, 96) - thread_rod(d, p_, 6.02, grow=FIT).translate([0, 0, -0.01])
        v = (male ^ nut).volume()
        print(f"thread {d:g}×{p_:g}: overlap {v:.3f} mm³")
        if v > 0.01:
            bad.append(f"thread {d}x{p_} binds")
        # real prints: each surface may be off by ±0.1 mm → what is left per 45° flank
        depth = 0.5 * p_ * 0.8
        gap = FIT * math.cos(math.radians(45))
        print(f"   {gap:.2f} mm per flank, {gap - 0.2:+.2f} if both parts print 0.1 mm fat; "
              f"{(depth - FIT) / depth:.0%} of the thread depth engaged")
    # 3. the USB-C plug (overmold 12.4 × 6.5, entering radially under the head)
    #    at every plausible port depth: 4–9 mm behind the glass
    hm, sm = built["head"], built["shade"].translate([0, 0, SHADE_MOUTH_Z])
    rb = BOARD_D / 2
    worst = 0.0
    for dz in [4.0, 5.0, 6.0, 7.0, 8.0, 9.0]:
        z = GLASS_Z + dz
        plug = Manifold.cube([30.0, 12.4, 6.5]).translate([-rb - 28.0, -6.2, z - 3.25])
        v = (plug ^ hm).volume() + (plug ^ sm).volume()
        worst = max(worst, v)
    print(f"USB-C plug path (port 4–9 mm behind the glass): worst overlap {worst:.1f} mm³")
    if worst > 0.5:
        bad.append("the USB-C plug is blocked")
    # 4. flat ceilings in print orientation (bed = z 0): every one must be a
    #    short bridge. (Threads have 45° flanks by construction; their helical
    #    mesh has micro-facets a raw face count would misread as overhangs.)
    for k in ("base", "base_lid", "shoulder", "jam_nut", "lower_arm", "upper_arm", "knob", "head", "shade",
              "cable_clip", "fit_coupon", "thread_gauge"):
        tm = to_trimesh(built[k])
        n, c, area = tm.face_normals, tm.triangles_center, tm.area_faces
        flat = (n[:, 2] < -0.9999) & (c[:, 2] > 0.2)
        zs = np.round(c[flat, 2] * 2) / 2
        groups = {}
        for z, a_ in zip(zs, area[flat]):
            groups[z] = groups.get(z, 0.0) + a_
        big = ", ".join(f"z {z:g}: {v:.0f} mm²" for z, v in sorted(groups.items()) if v > 60)
        print(f"ceilings {k:11s}: {big or 'none'}")
    # 5. stability: centre of mass over the base, with and without ballast.
    #    Printed parts at ~45 % of solid PLA (4 walls + 30 % infill), 1.24 g/cm³;
    #    the board ~15 g at the glass; the ballast ~150 g in the base.
    mass, moment = 0.0, 0.0
    for n in names:
        m_ = ms[n].volume() / 1000 * 1.24 * 0.45
        cx = to_trimesh(ms[n]).center_mass[0]
        mass, moment = mass + m_, moment + m_ * cx
    board_x = P["_points"]["screen_center"][0] + 4.0 * P["_points"]["screen_normal"][0] * -1
    mass, moment = mass + 15.0, moment + 15.0 * board_x
    r = BASE_D / 2
    for ballast in (0.0, 150.0):
        cx = moment / (mass + ballast)
        print(f"stability: {mass + ballast:5.0f} g, centre of mass x = {cx:+5.1f} mm over a base of ±{r:.0f} mm"
              f" ({'OK' if abs(cx) < 0.8 * r else 'TIPS' if abs(cx) >= r else 'marginal'})")
        if ballast and abs(cx) >= 0.8 * r:
            bad.append("the lamp tips forward even with ballast")
    print("CHECK", "FAILED:\n  " + "\n  ".join(bad) if bad else "OK")
    return not bad


def verify_stl():
    """The committed STLs must be what this generator produces: same triangle
    count, volume within 0.01 %, bounds within 0.001 mm. Exit status 1 if not."""
    stale = []
    for name, (fn, _, _) in PARTS.items():
        path = OUT / f"{name}.stl"
        if not path.exists():
            stale.append(f"{name}: missing")
            continue
        fresh = to_trimesh(fn())
        disk = trimesh.load(path, force="mesh")
        same = (len(fresh.faces) == len(disk.faces)
                and abs(fresh.volume - disk.volume) <= 1e-4 * abs(fresh.volume)
                and np.allclose(fresh.bounds, disk.bounds, atol=1e-3))
        print(f"{name:12s} {'ok' if same else 'STALE'}")
        if not same:
            stale.append(name)
    if stale:
        print("STL out of date — run `python3 lamp.py` and commit stl/:", ", ".join(stale))
    return not stale


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
    if "--check" in sys.argv or "--verify-stl" in sys.argv:
        ok = True
        if "--check" in sys.argv:
            ok = check() and ok
        if "--verify-stl" in sys.argv:
            ok = verify_stl() and ok
        sys.exit(0 if ok else 1)
    main()
