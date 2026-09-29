#!/usr/bin/env python
#Builds the models for Pot Sumo in Blender and saves them to a .blend file:
#  blender --background --python make-pots.py -- pots.blend
#(the meshes are then exported to dist/pots.pnct by export-meshes.py, see Makefile.windows / Makefile)
#
#Everything is modelled with bmesh: pots and the platform are surfaces of revolution ("lathe" / spin)
# of a hand-tuned profile curve, then coloured per-face with a vertex colour attribute (the game's
# shader multiplies vertex colours, so there are no textures or materials).

import sys, math, random
import bpy, bmesh
from mathutils import Vector, Matrix

args = []
for i in range(len(sys.argv)):
	if sys.argv[i] == '--':
		args = sys.argv[i+1:]
if len(args) != 1:
	print("Usage: blender --background --python make-pots.py -- <outfile.blend>")
	sys.exit(1)
outfile = args[0]

#start from an empty scene:
bpy.ops.wm.read_factory_settings(use_empty=True)
collection = bpy.data.collections.new("Meshes")
bpy.context.scene.collection.children.link(collection)

def lerp(a, b, t):
	return tuple(x + (y - x) * t for x, y in zip(a, b))

def catmull_rom(points, samples_per_segment):
	"""smooth curve through 2D points (endpoints are duplicated so the curve passes through them)"""
	pts = [points[0]] + list(points) + [points[-1]]
	out = []
	for i in range(1, len(pts) - 2):
		p0, p1, p2, p3 = pts[i-1], pts[i], pts[i+1], pts[i+2]
		for s in range(samples_per_segment):
			t = s / samples_per_segment
			t2, t3 = t*t, t*t*t
			out.append(tuple(0.5 * ((2*p1[k]) + (-p0[k] + p2[k]) * t + (2*p0[k] - 5*p1[k] + 4*p2[k] - p3[k]) * t2 + (-p0[k] + 3*p1[k] - 3*p2[k] + p3[k]) * t3) for k in range(2)))
	out.append(points[-1])
	return out

def spin_profile(bm, profile, steps):
	"""revolve a list of (r, z) points about the z axis (profile is a polyline, first point may be on the axis)"""
	verts = [bm.verts.new((r, 0.0, z)) for (r, z) in profile]
	edges = [bm.edges.new((verts[i], verts[i+1])) for i in range(len(verts) - 1)]
	res = bmesh.ops.spin(bm, geom=verts + edges, cent=(0, 0, 0), axis=(0, 0, 1), angle=2.0 * math.pi, steps=steps, use_duplicate=False)
	bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)

def torus(bm, major, minor, minor_scale_z, ring_steps, tube_steps, center, rotation):
	"""add a torus (axis = z before 'rotation') to bm"""
	before = set(bm.verts)
	circle = [bm.verts.new((major + minor * math.cos(a), 0.0, minor * minor_scale_z * math.sin(a))) for a in [2*math.pi*i/tube_steps for i in range(tube_steps)]]
	edges = [bm.edges.new((circle[i], circle[(i+1) % tube_steps])) for i in range(tube_steps)]
	bmesh.ops.spin(bm, geom=circle + edges, cent=(0, 0, 0), axis=(0, 0, 1), angle=2.0 * math.pi, steps=ring_steps, use_duplicate=False)
	new_verts = [v for v in bm.verts if v not in before]
	bmesh.ops.remove_doubles(bm, verts=new_verts, dist=1e-5)
	new_verts = [v for v in new_verts if v.is_valid]
	bmesh.ops.transform(bm, matrix=Matrix.Translation(center) @ rotation, verts=new_verts)

def finish_mesh(name, bm, color_fn, smooth):
	"""bm -> Blender mesh + object in the Meshes collection, with a per-corner colour attribute"""
	bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
	layer = bm.loops.layers.float_color.new("Color")
	for face in bm.faces:
		face.smooth = smooth
		c = color_fn(face.calc_center_median(), face.normal)
		for loop in face.loops:
			loop[layer] = (c[0], c[1], c[2], 1.0)
	mesh = bpy.data.meshes.new(name)
	bm.to_mesh(mesh)
	bm.free()
	mesh.color_attributes.active_color = mesh.color_attributes["Color"]
	obj = bpy.data.objects.new(name, mesh)
	collection.objects.link(obj)
	return obj

#----------------------------------------------------------------------------
# platform: plain flat cylinder, top face at z = 0, radius 1, 0.5 thick
# (the game scales x/y as it crumbles)

def make_platform():
	stone = (0.62, 0.58, 0.52)
	dark = (0.32, 0.29, 0.27)
	rune = (0.85, 0.72, 0.35)
	side = (0.42, 0.39, 0.36)
	rings = [(0.0, None), (0.18, rune), (0.24, stone), (0.55, stone), (0.60, dark), (0.66, stone), (0.90, stone), (0.94, rune), (1.0, stone)]

	#profile: across the top (split into rings so each band can have its own colour), down the side, across the flat bottom
	profile = [(r, 0.0) for r, _ in rings]
	profile += [(1.0, -0.5), (0.0, -0.5)]

	bm = bmesh.new()
	spin_profile(bm, profile, 64)

	def color(center, normal):
		if normal.z > 0.9:                                          #top face: concentric bands
			r = math.hypot(center.x, center.y)
			for outer, col in rings[1:]:
				if r <= outer + 1e-4:
					return col
			return stone
		return side                                                 #side wall + bottom
	return finish_mesh("Platform", bm, color, smooth=False)

#----------------------------------------------------------------------------
# pots: plain single-colour clay pots

PALETTE = [
	(0.86, 0.26, 0.22), #red
	(0.24, 0.52, 0.90), #blue
	(0.95, 0.75, 0.20), #yellow
	(0.30, 0.74, 0.40), #green
	(0.72, 0.40, 0.86), #purple
	(0.95, 0.52, 0.16), #orange
	(0.25, 0.80, 0.82), #teal
	(0.92, 0.45, 0.68), #pink
]

def make_pot(index, base):
	#profile, bottom to lip. The belly is a smooth spline; foot, lip and mouth are crisp.
	foot = [(0.0, 0.0), (0.24, 0.0), (0.30, 0.02)]
	belly = catmull_rom([(0.30, 0.02), (0.44, 0.14), (0.56, 0.34), (0.60, 0.52), (0.55, 0.70), (0.42, 0.84), (0.34, 0.92)], 5)
	lip = [(0.34, 0.92), (0.38, 0.95), (0.42, 0.975), (0.42, 1.0), (0.38, 1.015), (0.34, 1.0)]
	mouth = [(0.34, 1.0), (0.30, 0.97), (0.24, 0.93), (0.0, 0.90)]
	profile = foot + belly[1:] + lip[1:] + mouth[1:]

	bm = bmesh.new()
	spin_profile(bm, profile, 48)

	return finish_mesh("Pot%d" % index, bm, lambda center, normal: base, smooth=True)

#----------------------------------------------------------------------------
# water: big disc with soft waves, darker toward the horizon

def make_water():
	radius, ring_count, seg = 60.0, 34, 64
	bm = bmesh.new()
	radii = [0.0] + [0.6 * (radius / 0.6) ** (i / (ring_count - 1)) for i in range(1, ring_count)]
	rings = []
	for ri, r in enumerate(radii):
		ring = []
		count = 1 if ri == 0 else seg
		for s in range(count):
			a = 2 * math.pi * s / seg
			x, y = r * math.cos(a), r * math.sin(a)
			amp = min(1.0, r / 4.0) * 0.12
			z = amp * (math.sin(x * 1.3 + y * 0.7) + math.sin(y * 1.9 - x * 0.4) * 0.6)
			ring.append(bm.verts.new((x, y, z)))
		rings.append(ring)
	for ri in range(1, len(rings)):
		inner, outer = rings[ri-1], rings[ri]
		for s in range(seg):
			s2 = (s + 1) % seg
			if len(inner) == 1:
				bm.faces.new((inner[0], outer[s], outer[s2]))
			else:
				bm.faces.new((inner[s], outer[s], outer[s2], inner[s2]))
	shallow, deep = (0.16, 0.42, 0.55), (0.06, 0.18, 0.32)
	def color(center, normal):
		r = math.hypot(center.x, center.y)
		t = min(1.0, r / 40.0)
		return lerp(shallow, deep, t)
	return finish_mesh("Water", bm, color, smooth=True)

#----------------------------------------------------------------------------
# ring: rounded flat ring (outer radius 1), used for the "you" marker and splashes

def make_ring():
	bm = bmesh.new()
	torus(bm, 0.91, 0.09, 0.45, 48, 8, Vector((0, 0, 0)), Matrix.Identity(4))
	return finish_mesh("Ring", bm, lambda c, n: (1.0, 1.0, 1.0), smooth=True)

#----------------------------------------------------------------------------

make_platform()
for i, base in enumerate(PALETTE):
	make_pot(i, base)
make_water()
make_ring()

bpy.ops.wm.save_as_mainfile(filepath=bpy.path.abspath(outfile) if not outfile.startswith("//") else outfile)
print("Saved " + outfile + " with meshes: " + ", ".join(o.name for o in collection.objects))
