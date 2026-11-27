# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded dense-grid Warp port of V21 PIC/small-strain MPM.
All P2G, grid force/contact, G2P and stress/yield updates are device kernels.
Not yet APIC/MLS-MPM, sparse grid, native Blender or a fracture solver."""
import math
import numpy as np
import warp as wp

@wp.func
def base_node(x: wp.vec3, origin: wp.vec3, dx: float):
    q=(x-origin)/dx
    return wp.vec3i(int(wp.floor(q[0])),int(wp.floor(q[1])),int(wp.floor(q[2])))

@wp.func
def flat_id(a: wp.vec3i, n: int):
    return (a[0]*n+a[1])*n+a[2]

@wp.func
def inside(a: wp.vec3i, n: int):
    return a[0]>=0 and a[1]>=0 and a[2]>=0 and a[0]+1<n and a[1]+1<n and a[2]+1<n

@wp.func
def shape(f: wp.vec3, i: int, j: int, k: int):
    return wp.vec3(1.0-f[0] if i==0 else f[0],1.0-f[1] if j==0 else f[1],
                   1.0-f[2] if k==0 else f[2])

@wp.func
def grad(w: wp.vec3, i: int,j: int,k: int,dx: float):
    return wp.vec3(float(2*i-1)*w[1]*w[2],float(2*j-1)*w[0]*w[2],
                   float(2*k-1)*w[0]*w[1])/dx

@wp.kernel
def p2g(x: wp.array(dtype=wp.vec3),v: wp.array(dtype=wp.vec3),
        volume: wp.array(dtype=float),stress: wp.array(dtype=wp.mat33),
        mass: float,rest: float,dx: float,dt: float,bulk: float,shear: float,
        viscosity: float,origin: wp.vec3,n: int,
        gm: wp.array(dtype=float),mom: wp.array(dtype=wp.vec3),
        error: wp.array(dtype=wp.int32)):
    p=wp.tid()
    base=base_node(x[p],origin,dx)
    rho=mass/volume[p]
    speed=wp.length(v[p])+wp.sqrt((bulk+4.0*shear/3.0)/rho)
    if not wp.isfinite(speed) or not wp.isfinite(rho) or rho<=0.0 or dt*speed>0.25*dx or dt*viscosity/rho>0.1*dx*dx:
        wp.atomic_max(error,0,1)
    elif not inside(base,n):
        wp.atomic_max(error,0,2)
    else:
        f=(x[p]-origin)/dx-wp.vec3(float(base[0]),float(base[1]),float(base[2]))
        pressure=-bulk*wp.log(volume[p]/rest)
        sigma=stress[p]-wp.identity(n=3,dtype=float)*pressure
        for i in range(2):
            for j in range(2):
                for k in range(2):
                    w=shape(f,i,j,k)
                    weight=w[0]*w[1]*w[2]
                    g=grad(w,i,j,k,dx)
                    a=flat_id(base+wp.vec3i(i,j,k),n)
                    wp.atomic_add(gm,a,mass*weight)
                    wp.atomic_add(mom,a,v[p]*(mass*weight)-(sigma*g)*(volume[p]*dt))

@wp.kernel
def update_grid(gm: wp.array(dtype=float),mom: wp.array(dtype=wp.vec3),
                gv: wp.array(dtype=wp.vec3),n: int,origin: wp.vec3,dx: float,
                dt: float,gravity: wp.vec3,tool: int,center: wp.vec3,
                tool_velocity: wp.vec3,radius: float,friction: float,
                floor: int,z: float,floor_friction: float,error: wp.array(dtype=wp.int32)):
    a=wp.tid()
    u=wp.vec3(0.0,0.0,0.0)
    if gm[a]>1.0e-15:
        u=mom[a]/gm[a]+gravity*dt
        ix=a//(n*n)
        iy=(a//n)%n
        iz=a%n
        p=origin+wp.vec3(float(ix),float(iy),float(iz))*dx
        if tool!=0:
            d=p-center
            r=wp.length(d)
            if r<radius and r>1.0e-12:
                normal=d/r
                rel=u-tool_velocity
                vn=wp.dot(rel,normal)
                if vn<0.0:
                    tangent=rel-normal*vn
                    slip=wp.max(0.0,1.0-friction*(-vn)/wp.max(wp.length(tangent),1.0e-12))
                    u=tool_velocity+tangent*slip
        if floor!=0 and p[2]<=z+dx*0.5 and u[2]<0.0:
            u=wp.vec3(u[0]*(1.0-floor_friction),u[1]*(1.0-floor_friction),0.0)
        if not wp.isfinite(wp.length(u)):
            wp.atomic_max(error,0,3)
            u=wp.vec3(0.0,0.0,0.0)
    gv[a]=u

@wp.kernel
def g2p(x: wp.array(dtype=wp.vec3),volume: wp.array(dtype=float),
        stress: wp.array(dtype=wp.mat33),plastic: wp.array(dtype=float),
        gv: wp.array(dtype=wp.vec3),origin: wp.vec3,n: int,dx: float,dt: float,
        rest: float,bulk: float,shear: float,cohesion: float,friction: float,
        viscosity: float,floor: int,z: float,
        xn: wp.array(dtype=wp.vec3),vn: wp.array(dtype=wp.vec3),
        voln: wp.array(dtype=float),sn: wp.array(dtype=wp.mat33),
        pn: wp.array(dtype=float),error: wp.array(dtype=wp.int32)):
    p=wp.tid()
    base=base_node(x[p],origin,dx)
    if not inside(base,n):
        wp.atomic_max(error,0,2)
    else:
        f=(x[p]-origin)/dx-wp.vec3(float(base[0]),float(base[1]),float(base[2]))
        u=wp.vec3(0.0,0.0,0.0)
        L=wp.mat33(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0)
        for i in range(2):
            for j in range(2):
                for k in range(2):
                    w=shape(f,i,j,k)
                    vel=gv[flat_id(base+wp.vec3i(i,j,k),n)]
                    u+=vel*(w[0]*w[1]*w[2])
                    L+=wp.outer(vel,grad(w,i,j,k,dx))
        div=L[0,0]+L[1,1]+L[2,2]
        if not wp.isfinite(div) or wp.abs(div*dt)>0.25 or wp.length(u)*dt>0.5*dx:
            wp.atomic_max(error,0,1)
        else:
            q=x[p]+u*dt
            if floor!=0 and q[2]<z:
                q=wp.vec3(q[0],q[1],z)
                u=wp.vec3(u[0],u[1],wp.max(0.0,u[2]))
            vv=volume[p]*wp.exp(div*dt)
            rate=0.5*(L+wp.transpose(L))-wp.identity(n=3,dtype=float)*(div/3.0)
            s=(stress[p]+rate*(2.0*shear*dt))*(1.0/(1.0+viscosity*dt))
            mag=wp.sqrt(wp.ddot(s,s))
            threshold=cohesion+friction*wp.max(0.0,-bulk*wp.log(vv/rest))
            strain=plastic[p]
            if mag>threshold and mag>0.0:
                strain+=(mag-threshold)/wp.max(shear,1.0e-12)
                s=s*(threshold/mag)
            if (not wp.isfinite(vv) or vv<=0.0 or not wp.isfinite(mag) or
                not wp.isfinite(strain) or not wp.isfinite(wp.length(q)) or
                not inside(base_node(q,origin,dx),n)):
                wp.atomic_max(error,0,3)
            else:
                xn[p]=q
                vn[p]=u
                voln[p]=vv
                sn[p]=s
                pn[p]=strain

class MPM:
    def __init__(self,x,v=None,*,dx,particle_spacing,density=1.,origin=(-1.,-1.,-1.),n=32,device="cuda:0"):
        x=np.asarray(x,dtype=np.float32)
        v=np.zeros_like(x) if v is None else np.asarray(v,dtype=np.float32)
        if (x.ndim!=2 or x.shape[1]!=3 or len(x)==0 or v.shape!=x.shape or
            not np.isfinite(x).all() or not np.isfinite(v).all() or
            not all(math.isfinite(a) and a>0 for a in (dx,particle_spacing,density)) or
            len(origin)!=3 or not all(math.isfinite(a) for a in origin) or not 4<=n<=160):
            raise ValueError("invalid MPM grid/state")
        self.device=wp.get_device(device)
        self.count=len(x)
        self.dx=float(dx)
        self.rest=float(particle_spacing**3)
        self.mass=float(density*self.rest)
        self.origin=wp.vec3(*origin)
        self.n=int(n)
        self.x=wp.array(x,dtype=wp.vec3,device=self.device)
        self.v=wp.array(v,dtype=wp.vec3,device=self.device)
        self.volume=wp.full(self.count,self.rest,dtype=float,device=self.device)
        self.stress=wp.zeros(self.count,dtype=wp.mat33,device=self.device)
        self.plastic=wp.zeros(self.count,dtype=float,device=self.device)
        self.xn,self.vn=wp.empty_like(self.x),wp.empty_like(self.v)
        self.voln,self.sn,self.pn=wp.empty_like(self.volume),wp.empty_like(self.stress),wp.empty_like(self.plastic)
        self.gm=wp.zeros(n**3,dtype=float,device=self.device)
        self.mom=wp.zeros(n**3,dtype=wp.vec3,device=self.device)
        self.gv=wp.zeros_like(self.mom)
        self.error=wp.zeros(1,dtype=wp.int32,device=self.device)
    def step(self,dt,gravity=(0.,0.,-9.81),bulk=20.,shear=8.,cohesion=.5,
             friction=.5,viscosity=.1,tool=None,tool_velocity=(0.,0.,0.),floor=None):
        if (not math.isfinite(dt) or dt<=0 or
            not all(math.isfinite(a) and a>=0 for a in (bulk,shear,cohesion,friction,viscosity)) or
            len(gravity)!=3 or len(tool_velocity)!=3 or
            not all(math.isfinite(a) for a in (*gravity,*tool_velocity))):
            raise ValueError("invalid MPM settings")
        if tool is not None and (len(tool)!=4 or not all(math.isfinite(a) for a in tool) or tool[3]<=0):
            raise ValueError("invalid MPM tool")
        if floor is not None and not math.isfinite(floor):
            raise ValueError("invalid floor")
        self.error.zero_()
        self.gm.zero_()
        self.mom.zero_()
        wp.launch(p2g,dim=self.count,inputs=[self.x,self.v,self.volume,self.stress,self.mass,self.rest,
            self.dx,dt,bulk,shear,viscosity,self.origin,self.n,self.gm,self.mom,self.error],device=self.device)
        enabled=int(tool is not None)
        center=wp.vec3(*(tool[:3] if tool else (0.,0.,0.)))
        radius=float(tool[3] if tool else 1.)
        f=int(floor is not None)
        z=float(floor if floor is not None else 0.)
        wp.launch(update_grid,dim=self.n**3,inputs=[self.gm,self.mom,self.gv,self.n,self.origin,self.dx,dt,
            wp.vec3(*gravity),enabled,center,wp.vec3(*tool_velocity),radius,.5,f,z,.35,self.error],device=self.device)
        wp.launch(g2p,dim=self.count,inputs=[self.x,self.volume,self.stress,self.plastic,self.gv,self.origin,
            self.n,self.dx,dt,self.rest,bulk,shear,cohesion,friction,viscosity,f,z,
            self.xn,self.vn,self.voln,self.sn,self.pn,self.error],device=self.device)
        status=int(self.error.numpy()[0])
        if status:
            raise RuntimeError(f"MPM GPU rejected staged step: {status}")
        self.x,self.xn=self.xn,self.x
        self.v,self.vn=self.vn,self.v
        self.volume,self.voln=self.voln,self.volume
        self.stress,self.sn=self.sn,self.stress
        self.plastic,self.pn=self.pn,self.plastic

def fixture(device):
    x=np.array([[a*.1,b*.1,.2+c*.1] for a in range(3) for b in range(3) for c in range(3)],np.float32)
    s=MPM(x,dx=.2,particle_spacing=.1,origin=(-.8,-.8,-.8),n=16,device=device)
    # Zero forces: no motion, grid mass equals particle mass.
    s.step(.001,gravity=(0.,0.,0.))
    np.testing.assert_allclose(s.x.numpy(),x,atol=1e-6)
    np.testing.assert_allclose(s.gm.numpy().sum(),len(x)*s.mass,rtol=2e-6)
    # Uniform translation under gravity, no induced strain.
    s.step(.001)
    np.testing.assert_allclose(s.x.numpy(),x+np.array([0.,0.,-9.81e-6]),atol=2e-6)
    np.testing.assert_allclose(s.volume.numpy(),s.rest,rtol=2e-5)
    before=s.x.numpy().copy()
    try:
        s.step(1.)
    except RuntimeError:
        pass
    else:
        raise AssertionError("unstable trial accepted")
    np.testing.assert_array_equal(s.x.numpy(),before)
    print("MPM fixture passed",device)
