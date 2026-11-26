"""Experimental Warp backend; no Blender host integration."""
import numpy as np
import warp as wp

@wp.kernel
def smooth_csr(p: wp.array(dtype=wp.vec3), n: wp.array(dtype=wp.vec3),
               weight: wp.array(dtype=float), exposure: wp.array(dtype=float),
               offsets: wp.array(dtype=wp.int32), neighbors: wp.array(dtype=wp.int32),
               radius: float, transport: float, tangent: int,
               out: wp.array(dtype=wp.vec3)):
    i = wp.tid()
    out[i] = wp.vec3(0.0, 0.0, 0.0)
    if weight[i] > 0.0 and exposure[i] > 0.0:
        normal = wp.normalize(n[i])
        avg = wp.vec3(0.0, 0.0, 0.0)
        total = float(0.0)
        for k in range(offsets[i], offsets[i+1]):
            j = neighbors[k]
            if weight[j] > 0.0:
                a = wp.max(0.0, wp.dot(normal, wp.normalize(n[j])))
                avg += (p[j] - p[i]) * a
                total += a
        if total > 0.0:
            avg = avg / total
            if tangent != 0:
                avg -= normal * wp.dot(avg, normal)
            d = avg * (1.0 - wp.exp(-exposure[i] / radius * transport))
            length = wp.length(d)
            if length > radius * 0.1:
                d *= radius * 0.1 / length
            out[i] = d

class StrokeSession:
    """Fixed topology per session. Host owns undo and safe writeback."""
    def __init__(self, offsets, neighbors, device):
        offsets = np.asarray(offsets, dtype=np.int64)
        neighbors = np.asarray(neighbors, dtype=np.int64)
        if offsets.ndim != 1 or neighbors.ndim != 1 or len(offsets) < 2:
            raise ValueError("CSR shape")
        self.count = len(offsets) - 1
        if (self.count > 8192 or offsets[0] != 0 or offsets[-1] != len(neighbors)
            or np.any(np.diff(offsets) < 0) or np.any(neighbors < 0)
            or np.any(neighbors >= self.count) or len(neighbors) > 2**31-1):
            raise ValueError("CSR layout")
        self.device = wp.get_device(device)
        self.offsets = wp.array(offsets.astype(np.int32), dtype=wp.int32, device=self.device)
        self.neighbors = wp.array(neighbors.astype(np.int32), dtype=wp.int32, device=self.device)
        self.p = wp.zeros(self.count, dtype=wp.vec3, device=self.device)
        self.n = wp.zeros_like(self.p)
        self.weight = wp.zeros(self.count, dtype=float, device=self.device)
        self.exposure = wp.zeros_like(self.weight)
        self.out = wp.zeros_like(self.p)

    def evaluate(self, p, n, weight, exposure, radius, transport, tangent=True):
        if not np.isfinite(radius) or radius <= 0 or not np.isfinite(transport) or transport < 0:
            raise ValueError("material parameters")
        for dst, value, shape in ((self.p,p,(self.count,3)),(self.n,n,(self.count,3)),
                                  (self.weight,weight,(self.count,)),(self.exposure,exposure,(self.count,))):
            a = np.asarray(value, dtype=np.float32)
            if a.shape != shape or not np.all(np.isfinite(a)):
                raise ValueError("invalid input")
            if dst is self.exposure and np.any(a < 0):
                raise ValueError("negative exposure")
            dst.assign(a)
        wp.launch(smooth_csr, dim=self.count, inputs=[self.p,self.n,self.weight,
                  self.exposure,self.offsets,self.neighbors,float(radius),float(transport),
                  int(tangent),self.out], device=self.device)
        return self.out.numpy()
