#!/usr/bin/env python3
"""Convert Tux Racer 0.61 data files into ROM-resident C tables.

Sources (GPLv2, (c) 1999-2001 Jasmin F. Patry):
  tuxracer-data-0.61/courses/<course>/{course.tcl, elev.rgb, terrain.rgb, trees.rgb}
  tuxracer-data-0.61/courses/common/{courseinit.tcl, tux_walk.tcl}
  tuxracer-data-0.61/tux.tcl               (the Tux hierarchical model)

Outputs:
  src/gen/courses.c   heightmaps (u8), terrain type maps (2-bit packed),
                      tree/item lists (from trees.rgb, exact Tux Racer
                      colour-matching rules), course parameters.
  src/gen/tuxmodel.c  Tux model baked to ellipsoid parts with joints
                      (evaluated with real Tcl from tux.tcl).

The conversion follows course_load.c exactly:
  ELEV(nx-1-x, ny-1-y) = ((pix - base)/255)*elev_scale - (ny-1-y)/ny*length*tan(angle)
We keep the raw 8-bit pixel and let the runtime apply the formula in
fixed point, so the ROM holds nx*ny bytes per course.
"""
import math, os, re, struct, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.environ.get('TUXRACER_DATA', '/home/user/ref/tuxracer-data-0.61')
OUT_COURSES = os.path.join(ROOT, 'src', 'gen', 'courses.c')
OUT_TUX = os.path.join(ROOT, 'src', 'gen', 'tuxmodel.c')

# Canadian Cup + practice courses in the original menu order (course_idx.tcl)
COURSES = [
    # dir, display name, herring req (easy..insane), time req, par time
    ('bunny_hill',      'Bunny Hill',      [23, 23, 23, 23], [37, 35, 32, 30], 40.0),
    ('twisty_slope',    'Twisty Slope',    [24, 24, 24, 24], [43, 40, 34, 31.5], 40.0),
    ('bumpy_ride',      'Bumpy Ride',      [18, 18, 18, 18], [35, 30, 28, 27], 40.0),
    ('frozen_river',    'Frozen River',    [0, 0, 0, 0],     [80, 80, 80, 80], 80.0),
    ('path_of_daggers', 'Path of Daggers', [0, 0, 0, 0],     [70, 70, 70, 70], 70.0),
]

# tree/item specs from courses/common/courseinit.tcl
TREE_TYPES = [
    # name, colour, diam, height, size_varies
    ('tree3', (0, 255, 48),   1.4, 1.0, 0.5),
    ('tree1', (255, 255, 255), 1.4, 2.5, 0.5),
    ('tree2', (255, 96, 0),   1.4, 2.5, 0.5),
]
ITEM_TYPES = [
    # name, colour, diam, height, above_ground, collectable(1)/nocollision(-1), drawable, kind
    ('herring', (28, 185, 204), 1.0, 1.0, 0.2, 1, True),
    ('flag',    (194, 40, 40),  1.0, 1.0, 0.0, -1, True),
    ('finish',  (255, 255, 0),  9.0, 6.0, 0.0, -1, True),
    ('start',   (128, 128, 0),  9.0, 6.0, 0.0, -1, True),
    ('float',   (255, 128, 255), 0.0, 0.0, 0.0, -1, False),
]
ITEM_KIND = {'herring': 0, 'flag': 1, 'finish': 2, 'start': 3, 'float': 4}

ICE_IMG_VAL, ROCK_IMG_VAL, SNOW_IMG_VAL = 0, 127, 255
TERRAIN_ICE, TERRAIN_ROCK, TERRAIN_SNOW = 0, 1, 2


def parse_course_tcl(path):
    p = {'name': '', 'width': 0, 'length': 0, 'play_width': 0, 'play_length': 0,
         'start_x': 0, 'start_y': 0, 'angle': 20.0, 'elev_scale': 1.0, 'base_height': 127}
    with open(path, 'r', errors='replace') as f:
        for line in f:
            line = line.split(';#')[0].strip()
            if not line or line.startswith('#'):
                continue
            t = line.split()
            if t[0] == 'tux_course_name':
                p['name'] = ' '.join(t[1:]).strip('"')
            elif t[0] == 'tux_course_dim':
                p['width'], p['length'], p['play_width'], p['play_length'] = map(float, t[1:5])
            elif t[0] == 'tux_start_pt':
                p['start_x'], p['start_y'] = float(t[1]), float(t[2])
            elif t[0] == 'tux_angle':
                p['angle'] = float(t[1])
            elif t[0] == 'tux_elev_scale':
                p['elev_scale'] = float(t[1])
            elif t[0] == 'tux_base_height_value':
                p['base_height'] = int(t[1])
    return p


def load_rgb(path):
    """Load an SGI .rgb file via PIL; returns list of rows bottom-up like
    Tux Racer's image loader (row 0 = bottom of image = SGI row 0)."""
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    # PIL row 0 is the top of the image; SGI stores rows bottom-up and
    # Tux Racer indexes data[y] with y=0 at the SGI first row (bottom).
    rows = []
    for y in range(h):
        pil_row = h - 1 - y
        rows.append([px[x, pil_row] for x in range(w)])
    return w, h, rows


def intensity_to_terrain(v):
    d = {TERRAIN_ICE: abs(v - ICE_IMG_VAL), TERRAIN_SNOW: abs(v - SNOW_IMG_VAL), TERRAIN_ROCK: abs(v - ROCK_IMG_VAL)}
    best = TERRAIN_ICE
    m = d[TERRAIN_ICE]
    for k in (TERRAIN_ROCK, TERRAIN_SNOW):      # NUM_TERRAIN_TYPES order: Ice=0, Rock=1, Snow=2
        if d[k] < m:
            m = d[k]
            best = k
    return best


def compute_normals(nx, ny, elev, params):
    """Port of course_render.c calc_normals(): per-vertex normal = normalised sum
    of the unit normals of the surrounding triangles (alternating diagonals)."""
    W, L = params['width'], params['length']
    scale, base, slope = params['elev_scale'], params['base_height'], math.tan(math.radians(params['angle']))
    def P(x, y):
        return (x / (nx - 1.0) * W,
                (elev[x + nx * y] - base) / 255.0 * scale - y / float(ny) * L * slope,
                -y / (ny - 1.0) * L)
    def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
    def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    def norm(a):
        l = math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2) or 1.0
        return (a[0] / l, a[1] / l, a[2] / l)
    def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
    out = []
    for y in range(ny):
        for x in range(nx):
            p0 = P(x, y)
            nml = (0.0, 0.0, 0.0)
            def tri(a, b, flip=False):
                v1 = sub(a, p0); v2 = sub(b, p0)
                n = cross(v1, v2) if flip else cross(v2, v1)
                return norm(n)
            if (x + y) % 2 == 0:
                if x > 0 and y > 0:
                    nml = add(nml, tri(P(x, y - 1), P(x - 1, y - 1)))
                    nml = add(nml, tri(P(x - 1, y - 1), P(x - 1, y)))
                if x > 0 and y < ny - 1:
                    nml = add(nml, tri(P(x - 1, y), P(x - 1, y + 1)))
                    nml = add(nml, tri(P(x - 1, y + 1), P(x, y + 1)))
                if x < nx - 1 and y > 0:
                    nml = add(nml, tri(P(x + 1, y), P(x + 1, y - 1)))
                    nml = add(nml, tri(P(x + 1, y - 1), P(x, y - 1)))
                if x < nx - 1 and y < ny - 1:
                    nml = add(nml, tri(P(x + 1, y), P(x + 1, y + 1), True))
                    nml = add(nml, tri(P(x + 1, y + 1), P(x, y + 1), True))
            else:
                if x > 0 and y > 0:
                    nml = add(nml, tri(P(x, y - 1), P(x - 1, y)))
                if x > 0 and y < ny - 1:
                    nml = add(nml, tri(P(x - 1, y), P(x, y + 1)))
                if x < nx - 1 and y > 0:
                    nml = add(nml, tri(P(x + 1, y), P(x, y - 1)))
                if x < nx - 1 and y < ny - 1:
                    nml = add(nml, tri(P(x + 1, y), P(x, y + 1), True))
            n = norm(nml)
            if n[1] < 0:
                n = (-n[0], -n[1], -n[2])
            out.append(n)
    return out


def convert_course(cdir, params):
    nx, ny, elev_rows = load_rgb(os.path.join(cdir, 'elev.rgb'))
    tnx, tny, terr_rows = load_rgb(os.path.join(cdir, 'terrain.rgb'))
    assert (tnx, tny) == (nx, ny), 'terrain size mismatch'
    # ELEV(nx-1-x, ny-1-y) = f(elev_img[x + nx*y]); store elev_raw[idx] with idx=(nx-1-x)+nx*(ny-1-y)
    elev = [0] * (nx * ny)
    terr = [0] * (nx * ny)
    for y in range(ny):
        for x in range(nx):
            idx = (nx - 1 - x) + nx * (ny - 1 - y)
            elev[idx] = elev_rows[y][x][0]
            terr[idx] = intensity_to_terrain(terr_rows[y][x][0])
    # trees
    sx, sy, tree_rows = load_rgb(os.path.join(cdir, 'trees.rgb'))
    trees = []
    items = []
    W, L = params['width'], params['length']
    for y in range(sy):
        for x in range(sx):
            pix = tree_rows[y][x]
            # is_tree / is_item : manhattan distance, must beat pixel sum
            min_t = pix[0] + pix[1] + pix[2]
            which_t = None
            for i, (n, c, d, h, v) in enumerate(TREE_TYPES):
                dist = abs(c[0] - pix[0]) + abs(c[1] - pix[1]) + abs(c[2] - pix[2])
                if dist < min_t:
                    min_t = dist
                    which_t = i
            min_i = pix[0] + pix[1] + pix[2]
            which_i = None
            for i, it in enumerate(ITEM_TYPES):
                c = it[1]
                dist = abs(c[0] - pix[0]) + abs(c[1] - pix[1]) + abs(c[2] - pix[2])
                if dist < min_i:
                    min_i = dist
                    which_i = i
            px_ = (sx - x) / (sx - 1.0) * W
            pz_ = -(sy - y) / (sy - 1.0) * L
            if min_t < min_i and which_t is not None:
                trees.append((which_t, px_, pz_))
            elif which_i is not None:
                items.append((which_i, px_, pz_))
    normals = compute_normals(nx, ny, elev, params)
    return nx, ny, elev, terr, trees, items, normals


# deterministic LCG matching a fixed seed so sizes are reproducible
class LCG:
    def __init__(self, seed):
        self.s = seed & 0x7fffffff
    def rand(self):
        self.s = (self.s * 1103515245 + 12345) & 0x7fffffff
        return self.s / 0x7fffffff


def emit_courses():
    out = []
    out.append('/* Generated by tools/convert_data.py from tuxracer-data-0.61 (GPLv2). Do not edit. */')
    out.append('#include "../core/course.h"\n')
    course_entries = []
    for ci, (cname, disp, herring, times, par) in enumerate(COURSES):
        cdir = os.path.join(DATA, 'courses', cname)
        params = parse_course_tcl(os.path.join(cdir, 'course.tcl'))
        nx, ny, elev, terr, trees, items, normals = convert_course(cdir, params)
        # trees are sorted by type in Tux Racer (double pass); sizes randomised
        rng = LCG(1234 + ci)
        tree_list = []
        for ti in range(len(TREE_TYPES)):
            for (t, x, z) in trees:
                if t != ti:
                    continue
                name, col, diam, height, vary = TREE_TYPES[ti]
                hh = rng.rand() * vary * 2 - vary
                hh = height + hh * height
                dd = (hh / height) * diam
                tree_list.append((ti, x, z, hh, dd))
        # sort trees by z descending (near start first) for runtime culling
        tree_list.sort(key=lambda t: -t[2])
        item_list = []
        for ii in range(len(ITEM_TYPES)):
            for (t, x, z) in items:
                if t != ii:
                    continue
                name, col, diam, height, above, coll, drawable = ITEM_TYPES[ii]
                item_list.append((ITEM_KIND[name], x, z, diam, height, above, coll, drawable))
        item_list.sort(key=lambda t: -t[2])
        print('%-16s %dx%d trees=%d items=%d (herring=%d)' % (
            cname, nx, ny, len(tree_list), len(item_list), sum(1 for i in item_list if i[0] == 0)))
        out.append('static const u8 elev_%s[%d] = {' % (cname, nx * ny))
        for i in range(0, len(elev), 32):
            out.append('    ' + ','.join(str(v) for v in elev[i:i + 32]) + ',')
        out.append('};')
        # terrain 2 bits per sample, 4 per byte
        packed = []
        for i in range(0, len(terr), 4):
            b = 0
            for k in range(4):
                if i + k < len(terr):
                    b |= terr[i + k] << (2 * k)
            packed.append(b)
        # normals: store nx, nz as s8 (ny >= 0 is reconstructed at runtime)
        nrm = []
        for (a, b, c) in normals:
            nrm.extend([max(-127, min(127, int(round(a * 127)))), max(-127, min(127, int(round(c * 127))))])
        out.append('static const s8 nrm_%s[%d] = {' % (cname, len(nrm)))
        for i in range(0, len(nrm), 30):
            out.append('    ' + ','.join(str(v) for v in nrm[i:i + 30]) + ',')
        out.append('};')
        # per-vertex shade level 0..15 (palette.h shade_level(): light dir
        # (0.7071, 0.7071, 0), no negative light), baked so the terrain
        # renderer does not reconstruct normals at runtime.  Same quantised
        # normal as the s8 table so physics and rendering agree.
        shade = []
        for (a, b, c) in normals:
            qa = max(-127, min(127, int(round(a * 127)))) / 127.0
            qc = max(-127, min(127, int(round(c * 127)))) / 127.0
            qb = math.sqrt(max(0.0, 1.0 - qa * qa - qc * qc))
            d = max(0.0, (qa + qb) * 0.70710678)
            shade.append(max(0, min(15, int(math.floor(d * 15 + 0.5)))))
        out.append('static const u8 shade_%s[%d] = {' % (cname, len(shade)))
        for i in range(0, len(shade), 32):
            out.append('    ' + ','.join(str(v) for v in shade[i:i + 32]) + ',')
        out.append('};')
        out.append('static const u8 terr_%s[%d] = {' % (cname, len(packed)))
        for i in range(0, len(packed), 32):
            out.append('    ' + ','.join(str(v) for v in packed[i:i + 32]) + ',')
        out.append('};')
        out.append('static const tree_def_t trees_%s[%d] = {' % (cname, max(1, len(tree_list))))
        if not tree_list:
            out.append('    {0,0,0,0,0},')
        for (ti, x, z, hh, dd) in tree_list:
            out.append('    {%d, %d, %d, %d, %d},' % (ti, int(round(x * 32)), int(round(z * 32)),
                                                   int(round(hh * 256)), int(round(dd * 256))))
        out.append('};')
        out.append('static const item_def_t items_%s[%d] = {' % (cname, max(1, len(item_list))))
        if not item_list:
            out.append('    {0,0,0,0,0,0,0},')
        for (kind, x, z, diam, height, above, coll, drawable) in item_list:
            out.append('    {%d, %d, %d, %d, %d, %d, %d},' % (
                kind, int(round(x * 32)), int(round(z * 32)), int(round(diam * 256)),
                int(round(height * 256)), int(round(above * 256)), 1 if coll == 1 else 0))
        out.append('};')
        slope = math.tan(math.radians(params['angle']))
        course_entries.append(
            '    { "%s", %d, %d, FX(%g), FX(%g), FX(%g), FX(%g), FX(%g), FX(%g), FX(%g), FX(%g), %d, FX(%g), '
            'elev_%s, terr_%s, nrm_%s, shade_%s, trees_%s, %d, items_%s, %d, {%d,%d,%d,%d}, {FX(%g),FX(%g),FX(%g),FX(%g)}, FX(%g) },' % (
                disp, nx, ny, params['width'], params['length'], params['play_width'], params['play_length'],
                params['start_x'], -params['start_y'], params['angle'], params['elev_scale'],
                params['base_height'], slope,
                cname, cname, cname, cname, cname, len(tree_list), cname, len(item_list),
                herring[0], herring[1], herring[2], herring[3], times[0], times[1], times[2], times[3], par))
    out.append('const course_def_t course_defs[%d] = {' % len(COURSES))
    out.extend(course_entries)
    out.append('};')
    out.append('const int num_course_defs = %d;' % len(COURSES))
    with open(OUT_COURSES, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote', OUT_COURSES)


# --------------------------------------------------------------------------
# Tux model: evaluate tux.tcl with a real Tcl interpreter, capturing the
# scene graph commands, then bake each sphere node's cumulative transform
# below its nearest joint into an ellipsoid (centre, 3 axis vectors).

def convert_tux():
    import _tkinter
    tcl = _tkinter.create(None, '', 'Tk', 0, 0, 0, 0)
    nodes = {}      # name -> dict(parent, children[], mat(4x4 col-major list), geom, material, joint)
    materials = {}
    order = []
    joints = {}     # joint name -> node

    def ident():
        return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]

    def matmul(a, b):   # a*b, matrices as m[col][row] (OpenGL style) -> emulate alglib multiply_matrices
        r = [[0.0] * 4 for _ in range(4)]
        for i in range(4):
            for j in range(4):
                r[i][j] = sum(a[k][j] * b[i][k] for k in range(4))
        return r

    def rot(axis, deg):
        m = ident()
        s, c = math.sin(math.radians(deg)), math.cos(math.radians(deg))
        if axis == 'x':
            m[1][1] = c; m[2][1] = -s; m[1][2] = s; m[2][2] = c
        elif axis == 'y':
            m[0][0] = c; m[2][0] = s; m[0][2] = -s; m[2][2] = c
        else:
            m[0][0] = c; m[1][0] = -s; m[0][1] = s; m[1][1] = c
        return m

    def trans(x, y, z):
        m = ident(); m[3][0] = x; m[3][1] = y; m[3][2] = z; return m

    def scale(x, y, z):
        m = ident(); m[0][0] = x; m[1][1] = y; m[2][2] = z; return m

    def mk_node(parent, name):
        full = parent + ':' + name if parent != ':' else ':' + name
        if parent == ':' and name == 't1':
            full = ':t1'
        n = {'name': full, 'parent': parent if parent in nodes else None, 'children': [],
             'mat': ident(), 'geom': None, 'material': None, 'joint': None, 'res': 1.0}
        nodes[full] = n
        if parent in nodes:
            nodes[parent]['children'].append(full)
        order.append(full)
        return full

    def cmd_root(name): return ''
    def cmd_material(name, diff, spec, exp):
        materials[name] = tuple(float(v) for v in diff.split())
        return ''
    def cmd_transform(parent, child):
        mk_node(parent, child); return ''
    def cmd_sphere(parent, child, res):
        full = mk_node(parent, child)
        nodes[full]['geom'] = 'sphere'
        nodes[full]['res'] = float(res)
        return ''
    def cmd_surfprop(node, mat):
        nodes[node]['material'] = mat; return ''
    def cmd_scale(node, center, factors):
        cx, cy, cz = (float(v) for v in center.split())
        fx_, fy, fz = (float(v) for v in factors.split())
        n = nodes[node]
        n['mat'] = matmul(n['mat'], trans(-cx, -cy, -cz))
        n['mat'] = matmul(n['mat'], scale(fx_, fy, fz))
        n['mat'] = matmul(n['mat'], trans(cx, cy, cz))
        return ''
    def cmd_translate(node, vec):
        x, y, z = (float(v) for v in vec.split())
        nodes[node]['mat'] = matmul(nodes[node]['mat'], trans(x, y, z))
        return ''
    def cmd_rotate(node, axis, deg):
        nodes[node]['mat'] = matmul(nodes[node]['mat'], rot(axis, float(deg)))
        return ''
    def cmd_shadow(node, onoff): return ''
    def cmd_eye(node, side): return ''
    def mk_joint(jname):
        def f(node):
            joints[jname] = node
            nodes[node]['joint'] = jname
            return ''
        return f

    tcl.createcommand('tux_root_node', cmd_root)
    tcl.createcommand('tux_material', cmd_material)
    tcl.createcommand('tux_transform', cmd_transform)
    tcl.createcommand('tux_sphere', cmd_sphere)
    tcl.createcommand('tux_surfaceproperty', cmd_surfprop)
    tcl.createcommand('tux_scale', cmd_scale)
    tcl.createcommand('tux_translate', cmd_translate)
    tcl.createcommand('tux_rotate', cmd_rotate)
    tcl.createcommand('tux_shadow', cmd_shadow)
    tcl.createcommand('tux_eye', cmd_eye)
    for j in ['neck', 'head', 'left_shoulder', 'right_shoulder', 'left_hip', 'right_hip',
              'left_knee', 'right_knee', 'left_ankle', 'right_ankle', 'tail']:
        tcl.createcommand('tux_' + j, mk_joint(j))
    nodes[':'] = {'name': ':', 'parent': None, 'children': [], 'mat': ident(), 'geom': None,
                  'material': None, 'joint': None, 'res': 1.0}
    tcl.eval('source ' + os.path.join(DATA, 'tux.tcl'))

    # The root ':t1' is the translation node (set by set_tux_pos), ':t1:r1' the rotation
    # node (orientation).  Joint tree: root -> ... -> joint nodes.  For each sphere we
    # compute the transform from its nearest ancestor joint (or the rotation node r1)
    # down to the sphere.  Joints themselves get their transform from their parent
    # joint (or r1) to the joint node.
    JOINT_NAMES = ['neck', 'head', 'left_shoulder', 'right_shoulder', 'left_hip', 'right_hip',
                   'left_knee', 'right_knee', 'left_ankle', 'right_ankle', 'tail']
    root_rot = ':t1:r1'
    # Assign an index to each "frame": 0 = r1 (body), 1..11 = joints
    frames = [root_rot] + [joints[j] for j in JOINT_NAMES]
    frame_index = {n: i for i, n in enumerate(frames)}

    def path_from_root(name):
        p = []
        while name is not None and name != ':':
            p.append(name)
            name = nodes[name]['parent']
        return list(reversed(p))

    def cumulative(from_node, to_node):
        """Matrix of to_node's local frame relative to from_node's local frame
        (transform of from_node itself excluded, to_node's own included)."""
        p = path_from_root(to_node)
        i = p.index(from_node)
        m = ident()
        for n in p[i + 1:]:
            m = matmul(m, nodes[n]['mat'])
        return m

    frame_parent = {}
    frame_local = {}
    for fi, fn in enumerate(frames):
        if fi == 0:
            frame_parent[fi] = -1
            frame_local[fi] = nodes[fn]['mat']     # r1 carries the 0.35 model scale
            continue
        # nearest ancestor frame
        p = path_from_root(fn)
        anc = None
        for n in reversed(p[:-1]):
            if n in frame_index:
                anc = n
                break
        frame_parent[fi] = frame_index[anc]
        frame_local[fi] = cumulative(anc, fn)

    # spheres
    parts = []
    mat_names = ['white_penguin', 'black_penguin', 'beak_colour', 'nostril_colour', 'iris_colour']
    for n in order:
        nd = nodes[n]
        if nd['geom'] != 'sphere':
            continue
        p = path_from_root(n)
        anc = None
        for q in reversed(p[:-1]):
            if q in frame_index:
                anc = q
                break
        m = cumulative(anc, n)
        # inherit material from nearest ancestor with one
        mat = None
        for q in reversed(p):
            if nodes[q]['material'] is not None:
                mat = nodes[q]['material']
                break
        if mat is None:
            mat = 'black_penguin'
        # ellipsoid: centre = m*(0,0,0); axes = columns of the 3x3 part (m[col][row])
        centre = (m[3][0], m[3][1], m[3][2])
        ax = [(m[c][0], m[c][1], m[c][2]) for c in range(3)]
        parts.append((frame_index[anc], n, mat_names.index(mat), centre, ax, nd['res']))

    def emit_mat(m):
        # store 3x3 (row-major for our mat34: rows) + translation, in 16.16
        rows = []
        for r in range(3):
            rows.append('{%d,%d,%d}' % tuple(int(round(m[c][r] * 65536)) for c in range(3)))
        t = '{%d,%d,%d}' % tuple(int(round(m[3][r] * 65536)) for r in range(3))
        return '{{%s}, %s}' % (','.join(rows), t)

    # ------------------------------------------------------------------
    # Bake each ellipsoid into a small triangle mesh in its frame's local
    # space, with hidden-triangle removal against the other ellipsoids of
    # the same frame (the z-buffer would hide them anyway).  Two LODs.
    import numpy as np

    def inv3(m):
        return np.linalg.inv(np.array(m, dtype=float))

    part_data = []
    for (fi, name, mi, c, ax, res) in parts:
        A = np.array([[ax[0][0], ax[1][0], ax[2][0]],
                      [ax[0][1], ax[1][1], ax[2][1]],
                      [ax[0][2], ax[1][2], ax[2][2]]], dtype=float)   # columns = axes
        vol = abs(np.linalg.det(A))
        part_data.append({'frame': fi, 'name': name, 'mat': mi, 'c': np.array(c), 'A': A,
                          'Ainv': np.linalg.inv(A), 'vol': vol})

    def tessellate(segs, rings):
        """unit sphere: returns (points, tris) ; rings = number of interior latitude rings"""
        pts = [np.array([0.0, 1.0, 0.0])]
        for r in range(1, rings + 1):
            lat = math.pi * r / (rings + 1)
            y = math.cos(lat)
            rr = math.sin(lat)
            for sg in range(segs):
                a = 2 * math.pi * sg / segs + (math.pi / segs if (r & 1) else 0.0)
                pts.append(np.array([rr * math.cos(a), y, rr * math.sin(a)]))
        pts.append(np.array([0.0, -1.0, 0.0]))
        tris = []
        top = 0
        bot = len(pts) - 1
        def ring_idx(r, sg):
            return 1 + (r - 1) * segs + (sg % segs)
        for sg in range(segs):
            tris.append((top, ring_idx(1, sg + 1), ring_idx(1, sg)))
        for r in range(1, rings):
            for sg in range(segs):
                a, b = ring_idx(r, sg), ring_idx(r, sg + 1)
                c_, d = ring_idx(r + 1, sg), ring_idx(r + 1, sg + 1)
                # rings alternate offset by half a segment: pick the diagonal accordingly
                if r & 1:
                    tris.append((a, b, d)); tris.append((a, d, c_))
                else:
                    tris.append((a, b, c_)); tris.append((b, d, c_))
        for sg in range(segs):
            tris.append((bot, ring_idx(rings, sg), ring_idx(rings, sg + 1)))
        return pts, tris

    def tier(vol, lod):
        if lod == 0:
            if vol > 0.15: return (8, 3)
            if vol > 0.02: return (6, 3)
            if vol > 0.002: return (4, 2)
            return (4, 1)
        else:
            if vol > 0.15: return (6, 2)
            if vol > 0.02: return (4, 2)
            if vol > 0.002: return (4, 1)
            return None

    def inside(qi, p_world, margin=0.97):
        q = part_data[qi]
        u = q['Ainv'].dot(p_world - q['c'])
        return np.dot(u, u) < margin * margin

    def hidden(pi, unit_pts):
        """A triangle of ellipsoid P is hidden when the point of P's true
        surface under the triangle's centroid lies inside another ellipsoid
        of the same frame.  Testing the exact surface point (instead of the
        flat centroid) avoids removing both of two nearly coincident
        surfaces because of the tessellation's sagitta."""
        me = part_data[pi]
        u = sum(unit_pts) / 3.0
        u = u / (np.linalg.norm(u) or 1.0)
        p_world = me['c'] + me['A'].dot(u)
        for qi, q in enumerate(part_data):
            if qi == pi or q['frame'] != me['frame']:
                continue
            if inside(qi, p_world, 0.995):
                return True
        return False

    lod_meshes = []
    for lod in (0, 1):
        frame_verts = [[] for _ in frames]      # per frame: list of (x,y,z)
        frame_tris = [[] for _ in frames]       # per frame: (i0,i1,i2, mat, nx,ny,nz)
        for pi, pd in enumerate(part_data):
            t = tier(pd['vol'], lod)
            if t is None:
                continue
            segs, rings = t
            pts, tris = tessellate(segs, rings)
            world = [pd['c'] + pd['A'].dot(u) for u in pts]
            # normals: (A^-T u) normalised
            AinvT = pd['Ainv'].T
            nrm = []
            for u in pts:
                n = AinvT.dot(u)
                l = np.linalg.norm(n) or 1.0
                nrm.append(n / l)
            fv = frame_verts[pd['frame']]
            base = len(fv)
            fv.extend([tuple(w) for w in world])
            for (i0, i1, i2) in tris:
                if hidden(pi, [pts[i0], pts[i1], pts[i2]]):
                    continue
                # face normal from vertex normals (flat shading)
                n = nrm[i0] + nrm[i1] + nrm[i2]
                l = np.linalg.norm(n) or 1.0
                n = n / l
                # winding: counter-clockwise seen from outside (along n)
                g = np.cross(world[i1] - world[i0], world[i2] - world[i0])
                if np.dot(g, n) < 0:
                    i1, i2 = i2, i1
                frame_tris[pd['frame']].append((base + i0, base + i1, base + i2, pd['mat'], n))
        lod_meshes.append((frame_verts, frame_tris))
        print('tux lod %d: %d verts, %d tris' % (lod, sum(len(v) for v in frame_verts), sum(len(t) for t in frame_tris)))

    out = []
    out.append('/* Generated by tools/convert_data.py from tuxracer-data-0.61/tux.tcl (GPLv2). Do not edit. */')
    out.append('#include "../core/tuxmodel.h"\n')
    out.append('/* frames: 0=body(r1) then joints: ' + ', '.join('%d=%s' % (i + 1, j) for i, j in enumerate(JOINT_NAMES)) + ' */')
    out.append('const tux_frame_def_t tux_frame_defs[%d] = {' % len(frames))
    for fi in range(len(frames)):
        out.append('    { %d, %s },  /* %s */' % (frame_parent[fi], emit_mat(frame_local[fi]), frames[fi]))
    out.append('};')
    out.append('const int tux_num_frames = %d;' % len(frames))
    for lod, (frame_verts, frame_tris) in enumerate(lod_meshes):
        for fi in range(len(frames)):
            fv, ft = frame_verts[fi], frame_tris[fi]
            out.append('static const vec3 tux_l%d_f%d_v[%d] = {' % (lod, fi, max(1, len(fv))))
            if not fv:
                out.append('    {0,0,0},')
            for (x, y, z) in fv:
                out.append('    {%d,%d,%d},' % (int(round(x * 65536)), int(round(y * 65536)), int(round(z * 65536))))
            out.append('};')
            out.append('static const tux_tri_t tux_l%d_f%d_t[%d] = {' % (lod, fi, max(1, len(ft))))
            if not ft:
                out.append('    {0,0,0,0,{0,127,0}},')
            for (i0, i1, i2, m, n) in ft:
                out.append('    {%d,%d,%d,%d,{%d,%d,%d}},' % (i0, i1, i2, m,
                           int(round(n[0] * 127)), int(round(n[1] * 127)), int(round(n[2] * 127))))
            out.append('};')
        out.append('const tux_mesh_t tux_mesh_lod%d[%d] = {' % (lod, len(frames)))
        for fi in range(len(frames)):
            out.append('    { tux_l%d_f%d_v, %d, tux_l%d_f%d_t, %d },' % (lod, fi, len(frame_verts[fi]), lod, fi, len(frame_tris[fi])))
        out.append('};')
    with open(OUT_TUX, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('tux model: %d frames, %d parts' % (len(frames), len(parts)))
    print('wrote', OUT_TUX)


if __name__ == '__main__':
    os.makedirs(os.path.join(ROOT, 'src', 'gen'), exist_ok=True)
    emit_courses()
    convert_tux()
