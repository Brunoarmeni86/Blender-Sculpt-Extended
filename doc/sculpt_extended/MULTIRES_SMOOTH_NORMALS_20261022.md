# Multires Sculpt smooth shading

The duplicated quad layout used by Sculpt grid drawing must retain four distinct
vertex normals for smooth faces. The legacy normal buffer repeated the first
corner normal unless a Face Set display mode was enabled. The optional combined
geometry buffer repeated it unconditionally for smooth quads. Both paths now
write the corresponding CCG normal at each corner, using the existing position
and index ordering. Faces explicitly marked sharp retain their quad normal.

This changes shading only: positions, sculpt layers, brush calculations and
buffer sizes are unchanged. Source copies in blender-github and blender match.
Validation is limited to the Release compilation and diff checks; visual
confirmation on the user's model is pending. The user performs manual tests.

Only the shareable build is rebuilt for this candidate. Existing portable
directories and published GitHub assets retain the preceding build.
