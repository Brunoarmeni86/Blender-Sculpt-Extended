/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>

namespace blender::ed::sculpt_paint {
/* SCULPT_ADAPTIVE_STROKE_V2: One Euro position filter; pressure stays independent. */
struct AdaptiveStrokeFilter {
  double x=0, y=0, raw_x=0, raw_y=0, vx=0, vy=0, time=0;
  bool initialized=false;
  void reset(double px,double py,double now) {
    x=raw_x=px; y=raw_y=py; vx=vy=0; time=now; initialized=true;
  }
  static double alpha(double cutoff,double dt) {
    return 1.0/(1.0+1.0/(6.283185307179586*cutoff*dt));
  }
  void update(double px,double py,double now,double radius,int strength) {
    if (!initialized) {reset(px,py,now);return;}
    const double dt=std::clamp(now-time,0.0001,0.1);
    const double derivative_alpha=alpha(8.0,dt);
    vx+=derivative_alpha*((px-raw_x)/dt-vx);
    vy+=derivative_alpha*((py-raw_y)/dt-vy);
    const double base=strength==1?6.0:strength==2?1.5:3.0;
    const double beta=strength==1?6.0:strength==2?1.5:3.0;
    const double speed=std::hypot(vx,vy)/std::max(1.0,radius);
    const double a=alpha(base+beta*speed,dt);
    x+=a*(px-x); y+=a*(py-y);
    raw_x=px;raw_y=py;time=now;
  }
};

/* Event history and fractional arc-length phase are independent of emitted dabs. */
struct AdaptiveStrokeSampler {
  double x=0,y=0,pressure=0,residue=0;
  double phase=0,dose=0,travel=0;
  bool initialized=false;
  void reset(double px,double py,double p) {
    x=px;y=py;pressure=std::clamp(p,0.0,1.0);
    residue=phase=dose=travel=0;initialized=true;
  }
  template<typename Emit>
  int sample(double px,double py,double p,double spacing,bool interpolate,Emit emit) {
    if (!std::isfinite(px)||!std::isfinite(py)||!std::isfinite(p)||
        !std::isfinite(spacing)||spacing<=0) return 0;
    p=std::clamp(p,0.0,1.0);
    if (!initialized) {reset(px,py,p);return 0;}
    const double dx=px-x,dy=py-y,length=std::hypot(dx,dy);
    int count=0;
    if(length>0) {
      double along=spacing-residue;
      while(along<=length) {
        const double t=along/length;
        emit(x+dx*t,y+dy*t,interpolate?pressure+(p-pressure)*t:p);
        count++;along+=spacing;
      }
      residue=std::clamp(length-(along-spacing),0.0,spacing);
    }
    x=px;y=py;pressure=p;return count;
  }
  /* Gauss quadrature integrates normalized distance over a pressure segment.
   * The spacing callback returns {dab spacing, reference spacing} in pixels. */
  template<typename Spacing>
  static double integral(double a,double b,double length,double p0,double p1,
                         bool interpolate,Spacing spacing,bool reference) {
    constexpr double nodes[4]={0.1834346424956498,0.5255324099163290,
                                0.7966664774136267,0.9602898564975363};
    constexpr double weights[4]={0.3626837833783620,0.3137066458778873,
                                  0.2223810344533745,0.1012285362903763};
    const double mid=(a+b)*0.5,half=(b-a)*0.5;
    double sum=0;
    for(int i=0;i<4;i++) for(int sign=-1;sign<=1;sign+=2) {
      const double t=mid+sign*half*nodes[i];
      const double p=interpolate?p0+(p1-p0)*t:p1;
      const auto step=spacing(p);
      sum+=weights[i]/std::max(1.0,reference?step.reference:step.spacing);
    }
    return sum*half*length;
  }
  template<typename Spacing,typename Emit>
  int sample_adaptive(double px,double py,double p,bool interpolate,Spacing spacing,Emit emit) {
    if(!std::isfinite(px)||!std::isfinite(py)||!std::isfinite(p))return 0;
    p=std::clamp(p,0.0,1.0);
    if(!initialized){reset(px,py,p);return 0;}
    const double dx=px-x,dy=py-y,length=std::hypot(dx,dy),p0=pressure;
    int count=0;
    double begin=0;
    while(length>0 && begin<1) {
      const double available=integral(begin,1,length,p0,p,interpolate,spacing,false);
      if(phase+available<1.0-1e-10) {
        phase+=available;
        travel+=length*(1-begin);
        dose+=integral(begin,1,length,p0,p,interpolate,spacing,true);
        break;
      }
      const double needed=std::max(0.0,1.0-phase);
      double lo=begin,hi=1;
      for(int iteration=0;iteration<32;iteration++) {
        const double mid=(lo+hi)*0.5;
        if(integral(begin,mid,length,p0,p,interpolate,spacing,false)<needed)lo=mid;
        else hi=mid;
      }
      const double t=(lo+hi)*0.5;
      dose+=integral(begin,t,length,p0,p,interpolate,spacing,true);
      travel+=length*(t-begin);
      emit(x+dx*t,y+dy*t,interpolate?p0+(p-p0)*t:p,dose,1.0,travel);
      count++;phase=dose=travel=0;
      if(t<=begin)break;
      begin=t;
    }
    x=px;y=py;pressure=p;return count;
  }
  template<typename Emit>
  int finish(Emit emit) {
    if(phase<=1e-9)return 0;
    emit(x,y,pressure,dose,phase,travel);
    phase=dose=travel=0;return 1;
  }
};
} // namespace blender::ed::sculpt_paint
