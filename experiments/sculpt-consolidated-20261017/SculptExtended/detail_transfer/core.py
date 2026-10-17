"""Detail transfer mathematics; independent of Blender and mesh search."""
import math


def add(a, b):
    return tuple(a[i] + b[i] for i in range(3))


def sub(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def mul(a, s):
    return tuple(v * s for v in a)


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def finite(v):
    return len(v) == 3 and all(math.isfinite(x) for x in v)


def unit(v):
    if not finite(v):
        raise ValueError('Non-finite direction')
    length = math.sqrt(dot(v, v))
    if length <= 1e-15:
        raise ValueError('Degenerate direction')
    return mul(v, 1.0 / length)


def barycentric(p, a, b, c):
    ab, ac, ap = sub(b, a), sub(c, a), sub(p, a)
    d00, d01, d11 = dot(ab, ab), dot(ab, ac), dot(ac, ac)
    scale = max(d00, d11)
    denominator = d00*d11 - d01*d01
    if not math.isfinite(denominator) or scale <= 0 or denominator <= 1e-14*scale*scale:
        raise ValueError('Degenerate reference triangle')
    d20, d21 = dot(ap, ab), dot(ap, ac)
    v = (d11*d20-d01*d21)/denominator
    w = (d00*d21-d01*d20)/denominator
    weights = (1-v-w, v, w)
    # BVH gives a point on the triangle; tolerate only numerical edge error.
    if min(weights) < -1e-5 or max(weights) > 1.00001:
        raise ValueError('Hit outside the reference triangle')
    clipped = tuple(max(0.0, min(1.0, x)) for x in weights)
    total = sum(clipped)
    return tuple(x/total for x in clipped)


def interpolate(weights, a, b, c):
    return tuple(weights[0]*a[i]+weights[1]*b[i]+weights[2]*c[i] for i in range(3))


def rotate_between_normals(vector, source_normal, target_normal):
    """Shortest normal-aligning rotation; not a geodesic surface transport."""
    a, b = unit(source_normal), unit(target_normal)
    cosine = max(-1.0, min(1.0, dot(a, b)))
    if cosine > 1.0-1e-12:
        return tuple(vector)
    if cosine < -1.0+1e-10:
        axis_hint = min(((1.,0.,0.), (0.,1.,0.), (0.,0.,1.)), key=lambda x: abs(dot(a,x)))
        axis = unit(cross(a, axis_hint))
        return sub(mul(axis, 2*dot(axis, vector)), vector)
    axis = cross(a, b)
    # Rodrigues with the unnormalised cross axis; stable near parallel normals.
    return add(add(vector, cross(axis, vector)), mul(cross(axis, cross(axis, vector)), 1/(1+cosine)))


def transfer_delta(delta, source_normal, target_normal, *, distance, max_distance,
                   cosine_limit, intensity=1.0, mode='NORMAL', fade=True):
    """Return None for a rejected correspondence, otherwise a world-space delta."""
    if not finite(delta) or not all(math.isfinite(x) for x in
            (distance, max_distance, cosine_limit, intensity)):
        return None
    if distance < 0 or max_distance <= 0 or distance > max_distance:
        return None
    try:
        a, b = unit(source_normal), unit(target_normal)
    except ValueError:
        return None
    if dot(a, b) < cosine_limit:
        return None
    if mode == 'NORMAL':
        result = mul(b, dot(delta, a))
    elif mode == 'VECTOR':
        result = rotate_between_normals(delta, a, b)
    else:
        raise ValueError('Unknown transfer mode')
    influence = intensity
    if fade:
        t = min(1.0, distance/max_distance)
        influence *= 1.0-t*t*(3.0-2.0*t)
    result = mul(result, influence)
    return result if finite(result) else None
