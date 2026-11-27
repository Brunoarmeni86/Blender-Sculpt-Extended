# SPDX-License-Identifier: GPL-2.0-or-later
"""GPU-resident, compression-only V23 PBF baseline. Python host schedules
kernels, never reads particle arrays during an accepted frame. Not Blender UI."""
import math
import numpy as np
import warp as wp

@wp.func
def density_weight(r: float, h: float):
    x = wp.max(0.0, 1.0 - r*r/(h*h))
    return 315.0/(64.0*3.141592653589793*h*h*h)*x*x*x

@wp.func
def constraint_grad(d: wp.vec3, r: float, h: float, ratio: float):
    x = 1.0-r/h
    return d*(-45.0/(3.141592653589793*h*h*h*h)*x*x/r*ratio)

@wp.func
def collide(p: wp.vec3, tool: int, center: wp.vec3, radius: float,
            floor: int, z: float):
    q = p
    if tool != 0:
        d = q-center
        r = wp.length(d)
        if r < radius:
            n = wp.vec3(0.0,0.0,1.0)
            if r > 1.0e-12:
                n = d/r
            q = center+n*radius
    if floor != 0:
        q = wp.vec3(q[0],q[1],wp.max(q[2],z))
    return q

@wp.kernel
def predict(x: wp.array(dtype=wp.vec3), v: wp.array(dtype=wp.vec3),
            dt: float, gravity: wp.vec3, h: float, tool: int, center: wp.vec3,
            radius: float, floor: int, z: float, p: wp.array(dtype=wp.vec3),
            invalid: wp.array(dtype=wp.int32)):
    i = wp.tid()
    u = v[i]+gravity*dt
    length = wp.length(u)*dt
    if not wp.isfinite(length) or length > 0.5*h:
        wp.atomic_max(invalid,0,1)
        p[i]=x[i]
    else:
        p[i]=collide(x[i]+u*dt,tool,center,radius,floor,z)

@wp.kernel
def density_lambda(grid: wp.uint64, p: wp.array(dtype=wp.vec3), h: float,
                   mass: float, rho0: float, eps: float,
                   rho: wp.array(dtype=float), lam: wp.array(dtype=float),
                   invalid: wp.array(dtype=wp.int32)):
    i = wp.tid()
    q = p[i]
    rsum = mass*density_weight(0.0,h)
    gsum = wp.vec3(0.0,0.0,0.0)
    denom = float(0.0)
    query = wp.hash_grid_query(grid,q,h)
    j = int(0)
    while wp.hash_grid_query_next(query,j):
        if j != i:
            d = q-p[j]
            r = wp.length(d)
            if r < h:
                if r <= 1.0e-12:
                    wp.atomic_max(invalid,0,2)
                else:
                    g = constraint_grad(d,r,h,mass/rho0)
                    rsum += mass*density_weight(r,h)
                    gsum += g
                    denom += wp.dot(g,g)
    value = -wp.max(0.0,rsum/rho0-1.0)/(denom+wp.dot(gsum,gsum)+eps)
    if not wp.isfinite(value) or not wp.isfinite(rsum):
        wp.atomic_max(invalid,0,3)
        value = 0.0
        rsum = rho0
    rho[i]=rsum
    lam[i]=value

@wp.kernel
def corrections(grid: wp.uint64, p: wp.array(dtype=wp.vec3), h: float,
                ratio: float, lam: wp.array(dtype=float), dx: wp.array(dtype=wp.vec3),
                peak: wp.array(dtype=float), invalid: wp.array(dtype=wp.int32)):
    i = wp.tid()
    q = p[i]
    total = wp.vec3(0.0,0.0,0.0)
    query = wp.hash_grid_query(grid,q,h)
    j = int(0)
    while wp.hash_grid_query_next(query,j):
        if j != i:
            d=q-p[j]
            r=wp.length(d)
            if r > 1.0e-12 and r < h:
                total += constraint_grad(d,r,h,ratio)*(lam[i]+lam[j])
    size=wp.length(total)
    if not wp.isfinite(size):
        wp.atomic_max(invalid,0,3)
        total=wp.vec3(0.0,0.0,0.0)
        size=0.0
    dx[i]=total
    wp.atomic_max(peak,0,size)

@wp.kernel
def apply_correction(p: wp.array(dtype=wp.vec3), dx: wp.array(dtype=wp.vec3),
                     peak: wp.array(dtype=float), h: float, tool: int,
                     center: wp.vec3, radius: float, floor: int, z: float):
    i=wp.tid()
    scale=wp.min(1.0,0.15*h/wp.max(peak[0],1.0e-30))
    p[i]=collide(p[i]+dx[i]*scale,tool,center,radius,floor,z)

@wp.kernel
def velocities(x: wp.array(dtype=wp.vec3), p: wp.array(dtype=wp.vec3),
               dt: float, v: wp.array(dtype=wp.vec3)):
    i=wp.tid()
    v[i]=(p[i]-x[i])/dt

@wp.kernel
def viscosity(grid: wp.uint64, p: wp.array(dtype=wp.vec3),
              v: wp.array(dtype=wp.vec3), rho: wp.array(dtype=float),
              h: float, mass: float, coeff: float, out: wp.array(dtype=wp.vec3),
              invalid: wp.array(dtype=wp.int32)):
    i=wp.tid()
    q=p[i]
    dv=wp.vec3(0.0,0.0,0.0)
    query=wp.hash_grid_query(grid,q,h)
    j=int(0)
    while wp.hash_grid_query_next(query,j):
        if i != j:
            r=wp.length(q-p[j])
            if r < h:
                alpha=coeff*density_weight(r,h)*mass/wp.max(rho[i],rho[j])
                dv += (v[j]-v[i])*alpha
    u=v[i]+dv
    if not wp.isfinite(wp.length(u)) or not wp.isfinite(wp.length(q)):
        wp.atomic_max(invalid,0,3)
    out[i]=u

class PBF:
    def __init__(self, x, v=None, *, h, mass, rho0, device="cuda:0"):
        x=np.asarray(x,dtype=np.float32)
        v=np.zeros_like(x) if v is None else np.asarray(v,dtype=np.float32)
        if (x.ndim!=2 or x.shape[1]!=3 or len(x)==0 or v.shape!=x.shape or
                not np.isfinite(x).all() or not np.isfinite(v).all() or
                not all(math.isfinite(a) and a>0 for a in (h,mass,rho0))):
            raise ValueError("invalid PBF initial state")
        self.device=wp.get_device(device)
        self.n=len(x)
        self.h,self.mass,self.rho0=float(h),float(mass),float(rho0)
        self.x=wp.array(x,dtype=wp.vec3,device=self.device)
        self.v=wp.array(v,dtype=wp.vec3,device=self.device)
        self.p=wp.empty_like(self.x)
        self.next_v=wp.empty_like(self.v)
        self.raw_v=wp.empty_like(self.v)
        self.dx=wp.empty_like(self.x)
        self.rho=wp.empty(self.n,dtype=float,device=self.device)
        self.lam=wp.empty_like(self.rho)
        self.peak=wp.zeros(1,dtype=float,device=self.device)
        self.invalid=wp.zeros(1,dtype=wp.int32,device=self.device)
        self.grid=wp.HashGrid(64,64,64,device=self.device)
        self.grid.reserve(self.n)
    def step(self,dt,gravity=(0.,0.,-9.81),iterations=5,visc=.1,
             tool=None,floor=None,relaxation=1e-6):
        if not (math.isfinite(dt) and dt>0 and 1<=iterations<=32 and
                0<=visc<=1 and math.isfinite(relaxation) and relaxation>0 and
                len(gravity)==3 and all(math.isfinite(a) for a in gravity)):
            raise ValueError("invalid PBF settings")
        if tool is not None and (len(tool)!=4 or not all(math.isfinite(a) for a in tool) or tool[3]<=0):
            raise ValueError("invalid tool sphere")
        if floor is not None and not math.isfinite(floor):
            raise ValueError("invalid floor")
        if tool is not None and floor is not None and tool[2]-tool[3]<floor:
            raise ValueError("overlapping floor/tool constraints unsupported")
        center=wp.vec3(*(tool[:3] if tool else (0.,0.,0.)))
        contact=[int(tool is not None),center,float(tool[3] if tool else 1.),
                 int(floor is not None),float(floor if floor is not None else 0.)]
        def launch(k,args):
            wp.launch(k,dim=self.n,inputs=args,device=self.device)
        self.invalid.zero_()
        launch(predict,[self.x,self.v,dt,wp.vec3(*gravity),self.h,*contact,self.p,self.invalid])
        for _ in range(iterations):
            self.grid.build(self.p,self.h)
            launch(density_lambda,[self.grid.id,self.p,self.h,self.mass,self.rho0,relaxation,self.rho,self.lam,self.invalid])
            self.peak.zero_()
            launch(corrections,[self.grid.id,self.p,self.h,self.mass/self.rho0,self.lam,self.dx,self.peak,self.invalid])
            launch(apply_correction,[self.p,self.dx,self.peak,self.h,*contact])
        self.grid.build(self.p,self.h)
        launch(density_lambda,[self.grid.id,self.p,self.h,self.mass,self.rho0,relaxation,self.rho,self.lam,self.invalid])
        launch(velocities,[self.x,self.p,dt,self.raw_v])
        launch(viscosity,[self.grid.id,self.p,self.raw_v,self.rho,self.h,self.mass,visc,self.next_v,self.invalid])
        # Only one integer readback per substep. Swap only on success.
        status=int(self.invalid.numpy()[0])
        if status:
            raise RuntimeError(f"PBF GPU rejected staged step: {status}")
        self.x,self.p=self.p,self.x
        self.v,self.next_v=self.next_v,self.v

def fixture(device):
    x=np.array([[a*.1,b*.1,.2+c*.1] for a in range(3) for b in range(3) for c in range(3)],np.float32)
    # No compression test: ballistic motion and momentum preservation.
    s=PBF(x,h=.2,mass=.001,rho0=10.,device=device)
    s.step(.001)
    np.testing.assert_allclose(s.x.numpy(),x+np.array([0.,0.,-9.81e-6]),atol=2e-6)
    # Compressed interior: symmetric density correction and finite velocities.
    s=PBF(x,h=.2,mass=.001,rho0=.6,device=device)
    for _ in range(3):
        s.step(.001,gravity=(0.,0.,0.),visc=.1)
    assert np.isfinite(s.x.numpy()).all()
    np.testing.assert_allclose(s.x.numpy().mean(0),x.mean(0),atol=1e-5)
    before=s.x.numpy().copy()
    try:
        s.step(1.,gravity=(0.,0.,-100.))
    except RuntimeError:
        pass
    else:
        raise AssertionError("unstable trial accepted")
    np.testing.assert_array_equal(s.x.numpy(),before)
    print("PBF fixture passed",device)
