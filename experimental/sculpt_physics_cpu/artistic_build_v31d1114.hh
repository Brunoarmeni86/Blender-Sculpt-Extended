/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "artistic_geometry_v31d1110.hh"

namespace sculpt_physics::artistic_v31d1110 {
// Artist-oriented deposition cross sections. Modulate an existing resolved
// native target, not its mask or alpha. No new writable vertices, no invented
// grain, no all-source geodesic transport. These are surface shape profiles.
inline double build_shape(Material material,double cross,double along,double radial,bool moving){
 const double c=std::clamp(std::abs(cross),0.,1.);
 const double a=std::clamp(std::abs(along),0.,1.);
 const double r=std::clamp(radial,0.,1.);
 const double x=moving?c:r;
 const double x2=x*x;
 const double r2=r*r;
 switch(material){
 case Material::SoftClay: // broad rounded shoulder
  return .88+.50*x2*(2.-x2);
 case Material::FirmClay: // firm, pronounced central crest
  return .30+1.35*std::pow(1.-x2,3.);
 case Material::Mud: // low wide spread, less dominant center
  return .58+1.15*x2*(2.-x2);
 case Material::Wax: { // a narrow ribbon with a flattened top and abrupt shoulder
  const double t=std::clamp((x-.28)/.38,0.,1.);
  return (.16+1.25*(1.-t*t*(3.-2.*t)))*(1.-.18*a*a);
 }
 case Material::Putty: // cohesive bulb; rounder across both axes
  return .24+1.55*std::pow(1.-r2,2.);
 case Material::Flesh:
  return .65+.30*(1.-r2);
 case Material::Foam:
  return .50+.30*x2;
 case Material::Stone:
  return .30+1.10*std::pow(1.-x2,4.);
 case Material::Wood:
  return .18+1.35*std::pow(1.-x2,2.)*(.75+.25*a);
 }
 throw std::invalid_argument("build material");
}

inline std::vector<Vec3> build_profile_target(const h::Patch& patch,
 std::vector<Vec3> native,Material material,Vec3 center,Vec3 axis,Vec3 direction,
 double radius,const std::vector<double>& compaction,bool fast=false){
 const auto n=patch.p.size();
 if(native.size()!=n||compaction.size()!=n||!std::isfinite(radius)||radius<=0||
    !std::isfinite(h::norm(center))||!std::isfinite(h::norm(axis))||
    !std::isfinite(h::norm(direction))||h::norm(axis)<1e-12)
  throw std::invalid_argument("build profile inputs");
 axis=h::unit(axis);
 const Vec3 tangent=h::unit(direction-axis*h::dot(direction,axis));
 const bool moving=h::norm(tangent)>.5;
 const Vec3 transverse=h::unit(h::cross(axis,tangent));
 const auto p=profile(material);
 for(std::size_t i=0;i<n;++i){
  if(!std::isfinite(h::norm(native[i]))||!std::isfinite(compaction[i]))
   throw std::invalid_argument("build native target");
  if(patch.weight[i]<=0||h::norm(native[i])<=1e-15){
   native[i]={};continue;
  }
  const Vec3 q=(patch.p[i]-center)/radius;
  const Vec3 planar=q-axis*h::dot(q,axis);
  const double r=h::norm(planar);
  const double x=moving?h::dot(planar,transverse):r;
  const double a=moving?h::dot(planar,tangent):r;
  // Retain original direction and native weighted amplitude. The modifier
  // is bounded independently of strength, pressure, alpha and mask.
  const double gain=p.deposit*build_shape(material,x,a,r,moving)/
      (1.+p.hardening*std::clamp(compaction[i],0.,1.));
  native[i]=native[i]*gain;
 }
 return fast?accept_fast(patch,std::move(native)):h::optimize(patch,std::move(native),true);
}
}
