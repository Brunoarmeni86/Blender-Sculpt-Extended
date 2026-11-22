"""Persistent colored XPBD distance-constraint solver (CPU oracle + Warp CUDA).

This is a standalone *local physics proxy* prototype for normal sculpt
Grab/Clay/Smooth. No Blender PBVH/Multires/Undo writer exists here yet.

GPU optimizations:
- Upload the compact local proxy, bonds and inverse masses ONCE per session.
- Cache a greedy edge graph coloring (no atomics/data races within each color).
- Reuse GPU position and constraint-lambda arrays between brush dabs.
- Upload sparse hard target constraints per dab; only read back requested IDs.
- Execute colors serially, each color's independent bonds in parallel.
- Never silently use CUDA if a device is not installed.

Limitations: hard position targets are not compliant brush constraints; there
is no volume/bending/collision solver here. Numerical parity is approximate,
and many colors can cause high launch overhead. Measure end-to-end latency.
"""
from __future__ import annotations
import numpy as np

from warp_granular_probe import wp

if wp is not None:
    @wp.kernel
    def _apply_hard_targets(
        x: wp.array(dtype=wp.vec3),
        inv_mass: wp.array(dtype=wp.float32),
        ids: wp.array(dtype=wp.int32),
        goals: wp.array(dtype=wp.vec3),
    ):
        i = wp.tid()
        p = ids[i]
        if inv_mass[p] > 0.0:
            x[p] = goals[i]

    @wp.kernel
    def _xpbd_compliant_targets(
        x: wp.array(dtype=wp.vec3),
        inv_mass: wp.array(dtype=wp.float32),
        ids: wp.array(dtype=wp.int32),
        goals: wp.array(dtype=wp.vec3),
        compliance: wp.array(dtype=wp.float32),
        multipliers: wp.array(dtype=wp.vec3),
        h2: wp.float32,
    ):
        t = wp.tid()
        particle = ids[t]
        w = inv_mass[particle]
        alpha = compliance[t] / h2
        denominator = w + alpha
        if denominator <= 0.0:
            return
        c = x[particle] - goals[t]
        dl = (c * (-1.0) - multipliers[t] * alpha) / denominator
        x[particle] = x[particle] + dl * w
        multipliers[t] = multipliers[t] + dl

    @wp.kernel
    def _xpbd_color_bonds(
        x: wp.array(dtype=wp.vec3),
        inv_mass: wp.array(dtype=wp.float32),
        a: wp.array(dtype=wp.int32),
        b: wp.array(dtype=wp.int32),
        rest: wp.array(dtype=wp.float32),
        compliance: wp.array(dtype=wp.float32),
        lambdas: wp.array(dtype=wp.float32),
        start: wp.int32,
        h2: wp.float32,
    ):
        e = start + wp.tid()
        i = a[e]
        j = b[e]
        wi = inv_mass[i]
        wj = inv_mass[j]
        diff = x[j] - x[i]
        length = wp.length(diff)
        if length <= 1.0e-12:
            return
        alpha = compliance[e] / h2
        denominator = wi + wj + alpha
        if denominator <= 0.0:
            return
        dlambda = (-(length - rest[e]) - alpha * lambdas[e]) / denominator
        n = diff / length
        x[i] = x[i] - n * (wi * dlambda)
        x[j] = x[j] + n * (wj * dlambda)
        lambdas[e] = lambdas[e] + dlambda

    @wp.kernel
    def _read_local_vertices(
        x: wp.array(dtype=wp.vec3),
        ids: wp.array(dtype=wp.int32),
        out: wp.array(dtype=wp.vec3),
    ):
        i = wp.tid()
        out[i] = x[ids[i]]


def _checked_indices(ids, count):
    vals = np.asarray(ids)
    if vals.ndim != 1:
        raise ValueError("vertex IDs must be 1-D")
    if len(vals) == 0:
        return np.empty(0,dtype=np.int32)
    if not np.issubdtype(vals.dtype,np.integer):
        raise ValueError("expected integer vertex IDs")
    if np.any(vals < 0) or np.any(vals >= count):
        raise IndexError("vertex ID outside local proxy")
    if len(np.unique(vals)) != len(vals):
        raise ValueError("duplicate local vertex ID")
    return np.asarray(vals,dtype=np.int32)


def _edge_colors(edges, particle_count):
    """One color per bond, no shared endpoint per color; stable ordering."""
    used = [set() for _ in range(particle_count)]
    colors = np.zeros(len(edges),dtype=np.int32)
    for k,(a,b) in enumerate(edges):
        color = 0
        while color in used[a] or color in used[b]:
            color += 1
        used[a].add(color)
        used[b].add(color)
        colors[k] = color
    if not len(edges):
        return np.empty(0,dtype=np.int32), np.array([0],dtype=np.int32)
    order = np.argsort(colors,kind="stable")
    counts = np.bincount(colors,minlength=int(np.max(colors))+1)
    offsets = np.concatenate([[0],np.cumsum(counts)]).astype(np.int32)
    return order.astype(np.int32),offsets


class LocalXpbdSession:
    """Fixed-topology, local XPBD spring proxy. Instantiate once per island.

    The input positions are copied; all calls are independent of Blender Mesh.
    Boundary vertices should have inverse_mass=0.0. Undo and edits must be
    staged in a separate native editor transaction before Blender writeback.
    """

    def __init__(self, positions, inverse_masses, edges, *,
                 compliance=0.0, rest_lengths=None, device="auto", target_mode="hard"):
        p=np.asarray(positions,dtype=np.float32)
        mass=np.asarray(inverse_masses,dtype=np.float32)
        pair=np.asarray(edges)
        if (p.ndim!=2 or p.shape[1]!=3 or mass.shape!=(len(p),) or
                pair.ndim!=2 or pair.shape[1]!=2 or
                not np.issubdtype(pair.dtype,np.integer) or
                not np.isfinite(p).all() or not np.isfinite(mass).all() or
                np.any(mass<0) or len(p)>np.iinfo(np.int32).max or
                len(pair)>np.iinfo(np.int32).max):
            raise ValueError("invalid XPBD local proxy")
        pair=np.asarray(pair,dtype=np.int64)
        if (np.any(pair<0) or np.any(pair>=len(p)) or
                np.any(pair[:,0]==pair[:,1])):
            raise ValueError("invalid/self-connected bond")
        if device not in ("auto","cpu","cuda","warp-cpu"):
            raise ValueError("device must be cpu, warp-cpu, cuda, or auto")
        available=bool(wp is not None and wp.get_cuda_device_count()>0)
        if device=="cuda" and not available:
            raise RuntimeError("Warp CUDA unavailable")
        if device=="warp-cpu" and wp is None:
            raise RuntimeError("Warp CPU runtime unavailable (install warp-lang)")
        self._cuda=(device=="cuda" or (device=="auto" and available))
        self._warp=self._cuda or device=="warp-cpu"
        if target_mode not in ("hard","compliant"):
            raise ValueError("target_mode must be 'hard' or 'compliant'")
        self._target_mode=target_mode
        self._n=len(p)
        self._m=len(pair)
        c=np.asarray(compliance,dtype=np.float32)
        if c.ndim==0:
            c=np.full(len(pair),float(c),dtype=np.float32)
        if c.shape!=(len(pair),) or not np.isfinite(c).all() or np.any(c<0):
            raise ValueError("invalid XPBD compliance")
        order, self._offsets=_edge_colors(pair,len(p))
        pair=pair[order].astype(np.int32)
        self._a=pair[:,0].copy()
        self._b=pair[:,1].copy()
        # Keep the C++ World's plastic/rest bond lengths when provided.
        # Rebuilding them from deformed positions would erase plastic memory.
        if rest_lengths is None:
            self._rest=np.linalg.norm(p[self._b]-p[self._a],axis=1).astype(np.float32)
        else:
            source_rest=np.asarray(rest_lengths,dtype=np.float32)
            if source_rest.shape!=(self._m,):
                raise ValueError("expected one rest length per proxy bond")
            self._rest=np.ascontiguousarray(source_rest[order])
        if not np.isfinite(self._rest).all() or np.any(self._rest<=1.0e-12):
            raise ValueError("degenerate proxy bond")
        self._compliance=c[order]
        self._inv=mass.copy()
        self._goals_ids=np.empty(0,dtype=np.int32)
        self._goals=np.empty((0,3),dtype=np.float32)
        self._goal_compliances=np.empty(0,dtype=np.float32)
        self._sparse_h2d=0
        self._sparse_d2h=0
        self._initial_h2d=0
        self._target_capacity=0
        self._read_capacity=0
        self._sparse_device_allocations=0
        if self._warp:
            wp.init()
            self._dev="cuda:0" if self._cuda else "cpu"
            self._x=wp.array(p,dtype=wp.vec3,device=self._dev)
            self._inv_gpu=wp.array(self._inv,dtype=wp.float32,device=self._dev)
            self._a_gpu=wp.array(self._a,dtype=wp.int32,device=self._dev)
            self._b_gpu=wp.array(self._b,dtype=wp.int32,device=self._dev)
            self._rest_gpu=wp.array(self._rest,dtype=wp.float32,device=self._dev)
            self._comp_gpu=wp.array(self._compliance,dtype=wp.float32,device=self._dev)
            self._lam_gpu=wp.zeros(self._m,dtype=wp.float32,device=self._dev)
            self._initial_h2d=(sum(v.nbytes for v in
               (p,self._inv,self._a,self._b,self._rest,self._compliance))
               if self._cuda else 0)
        else:
            self._x_cpu=p.copy()

    @property
    def device(self):
        return "cuda" if self._cuda else ("warp-cpu" if self._warp else "cpu")

    @property
    def num_colors(self):
        return len(self._offsets)-1

    @property
    def transfer_bytes(self):
        return (self._initial_h2d,self._sparse_h2d,self._sparse_d2h)

    def _reserve_targets(self, count):
        # Target IDs remain intact while read_positions() performs independent
        # readback operations. Do NOT share this index buffer with readback.
        if not self._warp or count <= self._target_capacity:
            return
        cap=max(8,count,self._target_capacity*2)
        ids_gpu=wp.empty(cap,dtype=wp.int32,device=self._dev)
        goals_gpu=wp.empty(cap,dtype=wp.vec3,device=self._dev)
        self._ids_gpu=ids_gpu
        self._goals_gpu=goals_gpu
        if self._target_mode=="compliant":
            # Persistent per-target compliance and XPBD lambda array.
            self._target_comp_gpu=wp.empty(cap,dtype=wp.float32,device=self._dev)
            self._target_lam_gpu=wp.zeros(cap,dtype=wp.vec3,device=self._dev)
        self._target_capacity=cap
        self._sparse_device_allocations+=4 if self._target_mode=="compliant" else 2

    def _reserve_readback(self, count):
        if not self._warp or count <= self._read_capacity:
            return
        cap=max(8,count,self._read_capacity*2)
        ids_gpu=wp.empty(cap,dtype=wp.int32,device=self._dev)
        output_gpu=wp.empty(cap,dtype=wp.vec3,device=self._dev)
        self._read_ids_gpu=ids_gpu
        self._readback_gpu=output_gpu
        self._read_capacity=cap
        self._sparse_device_allocations+=2

    @property
    def sparse_device_allocations(self):
        return self._sparse_device_allocations

    @property
    def scratch_capacities(self):
        return (self._target_capacity,self._read_capacity)

    def set_targets(self, indices, goals, *, target_compliance=0.0):
        # Validate all inputs before modifying resident targets or buffers.
        ids=_checked_indices(indices,self._n)
        xyz=np.asarray(goals,dtype=np.float32)
        if xyz.shape!=(len(ids),3) or not np.isfinite(xyz).all():
            raise ValueError("invalid brush target")
        c=np.asarray(target_compliance,dtype=np.float32)
        if c.ndim==0:
            c=np.full(len(ids),float(c),dtype=np.float32)
        if c.shape!=(len(ids),) or not np.isfinite(c).all() or np.any(c<0):
            raise ValueError("invalid XPBD target compliance")
        if self._target_mode=="hard" and np.any(c>0):
            raise ValueError("hard target mode cannot use nonzero compliance")
        if len(ids) and np.any(self._inv[ids]<=0):
            raise ValueError("cannot target a pinned local vertex")
        if self._warp and len(ids):
            # Keep geometry and sparse target buffers resident between dabs.
            self._reserve_targets(len(ids))
            wp.copy(self._ids_gpu,
                    wp.array(ids,dtype=wp.int32,device="cpu"),count=len(ids))
            wp.copy(self._goals_gpu,
                    wp.array(xyz,dtype=wp.vec3,device="cpu"),count=len(ids))
            if self._target_mode=="compliant":
                wp.copy(self._target_comp_gpu,
                        wp.array(c,dtype=wp.float32,device="cpu"),count=len(ids))
            if self._cuda:
                self._sparse_h2d+=ids.nbytes+xyz.nbytes
                if self._target_mode=="compliant":
                    self._sparse_h2d+=c.nbytes
        self._goals_ids=ids.copy()
        self._goals=xyz.copy()
        self._goal_compliances=c.copy()

    def solve(self, *, dt=1.0/60.0, iterations=5):
        if (not np.isfinite(dt) or dt<=0 or
                not isinstance(iterations,int) or not 1<=iterations<=256):
            raise ValueError("invalid XPBD step")
        h2=float(dt*dt)
        if not np.isfinite(h2) or h2<=0:
            raise ValueError("XPBD timestep underflow/overflow")
        if self._warp:
            self._lam_gpu.zero_()  # reset bond multipliers between separate dabs
            if self._target_mode=="compliant" and len(self._goals_ids):
                self._target_lam_gpu.zero_()  # one XPBD target lambda per dab
            if self._target_mode=="hard" and len(self._goals_ids):
                wp.launch(_apply_hard_targets,dim=len(self._goals_ids),
                    inputs=[self._x,self._inv_gpu,self._ids_gpu,
                            self._goals_gpu],device=self._dev)
            for _ in range(iterations):
                # Match World: brush targets first, then distance bonds.
                if self._target_mode=="compliant" and len(self._goals_ids):
                    wp.launch(_xpbd_compliant_targets,dim=len(self._goals_ids),
                        inputs=[self._x,self._inv_gpu,self._ids_gpu,
                                self._goals_gpu,self._target_comp_gpu,
                                self._target_lam_gpu,h2],device=self._dev)
                for color in range(self.num_colors):
                    start,end=map(int,self._offsets[color:color+2])
                    if end==start: continue
                    wp.launch(_xpbd_color_bonds,dim=end-start,
                        inputs=[self._x,self._inv_gpu,self._a_gpu,
                                self._b_gpu,self._rest_gpu,self._comp_gpu,
                                self._lam_gpu,start,h2],device=self._dev)
                if self._target_mode=="hard" and len(self._goals_ids):
                    wp.launch(_apply_hard_targets,dim=len(self._goals_ids),
                        inputs=[self._x,self._inv_gpu,self._ids_gpu,
                                self._goals_gpu],device=self._dev)
            return
        lam=np.zeros(self._m,dtype=np.float32)
        if len(self._goals_ids):
            self._x_cpu[self._goals_ids]=self._goals
        for _ in range(iterations):
            for color in range(self.num_colors):
                start,end=map(int,self._offsets[color:color+2])
                for k in range(start,end):
                    a,b=int(self._a[k]),int(self._b[k])
                    diff=self._x_cpu[b]-self._x_cpu[a]
                    distance=float(np.linalg.norm(diff))
                    if distance<=1.0e-12: continue
                    wi,wj=float(self._inv[a]),float(self._inv[b])
                    alpha=float(self._compliance[k])/h2
                    denom=wi+wj+alpha
                    if denom<=0: continue
                    dl=(-(distance-float(self._rest[k]))-alpha*float(lam[k]))/denom
                    direction=diff/distance
                    self._x_cpu[a]-=direction*(wi*dl)
                    self._x_cpu[b]+=direction*(wj*dl)
                    lam[k]+=dl
            if len(self._goals_ids):
                self._x_cpu[self._goals_ids]=self._goals
        if not np.isfinite(self._x_cpu).all():
            raise FloatingPointError("non-finite CPU XPBD positions")

    def read_positions(self, indices):
        ids=_checked_indices(indices,self._n)
        if not len(ids):
            return np.empty((0,3),dtype=np.float32)
        if not self._warp:
            return self._x_cpu[ids].copy()
        self._reserve_readback(len(ids))
        wp.copy(self._read_ids_gpu,
                wp.array(ids,dtype=wp.int32,device="cpu"),count=len(ids))
        wp.launch(_read_local_vertices,dim=len(ids),
                  inputs=[self._x,self._read_ids_gpu],
                  outputs=[self._readback_gpu],device=self._dev)
        out=self._readback_gpu[:len(ids)].numpy()
        if self._cuda:
            self._sparse_h2d+=ids.nbytes
            self._sparse_d2h+=out.nbytes
        if not np.isfinite(out).all():
            raise FloatingPointError("invalid CUDA XPBD result; refuse writeback")
        return out
