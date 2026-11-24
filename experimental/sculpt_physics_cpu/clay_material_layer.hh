/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sculpt_physics {

/* V19-A: conservative scalar clay layer over a stable surface chart.
 * Each cell owns added material volume, not vertex displacement.
 * The host supplies actual dual areas and surface anchors. The algorithm
 * is independent of Mesh/BMesh/CCG and does not edit topology. The caller
 * stages the returned layer and commits only after successful editor
 * writeback. A topology change requires conservative state migration.
 * This thin-layer approximation cannot represent overhangs, detached
 * blobs, or bulk fracture; a volumetric backend can share the action
 * contract later. */
struct ClayLayerCell {
  Vec3 position{};
  Vec3 normal{0,0,1};
  double area=1.0;
  double volume=0.0;
  double influence=1.0; // brush envelope, including mask
  bool writable=true;
};
struct ClayLayerEdge { std::size_t a=0,b=0; };
enum class ClayLayerAction { Deposit, Push, Gather, Spread };
struct ClayLayerDab {
  ClayLayerAction action=ClayLayerAction::Deposit;
  Vec3 center{};
  Vec3 direction{};
  double radius=1.0;
  double amount=0.0; // Deposit: total volume; others: dimensionless transport dose [0,1].
  double mobility=0.6; // normalized material mobility
};
struct ClayLayerResult {
  std::vector<double> volume;
  std::vector<Vec3> displacement; // incremental thickness along local normal
  double added_volume=0.0;
  double transported_volume=0.0;
};

inline ClayLayerResult stage_clay_layer(const std::vector<ClayLayerCell> &cells,
                                       std::vector<ClayLayerEdge> edges,
                                       const ClayLayerDab &dab)
{
  const auto norm=[](Vec3 v) {return std::hypot(v.x,v.y,v.z);};
  const auto dot=[](Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto unit=[&](Vec3 v) {const double l=norm(v);return l>1e-12 ? v/l : Vec3{};};
  const auto interval=[](double x) {return std::isfinite(x) && x>=0 && x<=1;};
  if (!std::isfinite(norm(dab.center)) || !std::isfinite(norm(dab.direction)) ||
      !std::isfinite(dab.radius) || dab.radius<=0 ||
      !std::isfinite(dab.amount) || dab.amount<0 ||
      !interval(dab.mobility) ||
      (dab.action!=ClayLayerAction::Deposit && dab.amount>1)) {
    throw std::invalid_argument("Invalid clay dab");
  }
  switch (dab.action) {
    case ClayLayerAction::Deposit: case ClayLayerAction::Push:
    case ClayLayerAction::Gather: case ClayLayerAction::Spread: break;
    default: throw std::invalid_argument("Invalid clay action");
  }
  const std::size_t n=cells.size();
  ClayLayerResult out;
  out.volume.reserve(n);
  out.displacement.resize(n);
  std::vector<double> weights(n),height(n);
  double weight_sum=0.0,total=0.0;
  for (std::size_t i=0;i<n;++i) {
    const auto &c=cells[i];
    if (!std::isfinite(norm(c.position)) || !std::isfinite(norm(c.normal)) ||
        norm(c.normal)<=1e-12 || !std::isfinite(c.area) || c.area<=0 ||
        !std::isfinite(c.volume) || c.volume<0 || !interval(c.influence)) {
      throw std::invalid_argument("Invalid clay cell");
    }
    height[i]=c.volume/c.area;
    if (!std::isfinite(height[i])) throw std::overflow_error("Invalid clay thickness");
    weights[i]=c.writable && norm(c.position-dab.center)<=dab.radius ?
                   c.influence : 0.0;
    weight_sum+=weights[i]*c.area;
    total+=c.volume;
    out.volume.push_back(c.volume);
  }
  if (!std::isfinite(weight_sum) || !std::isfinite(total))
    throw std::overflow_error("Clay layer budget overflow");
  for (auto &e : edges) {
    if (e.a>=n || e.b>=n || e.a==e.b)
      throw std::invalid_argument("Invalid clay adjacency");
    if (e.a>e.b) std::swap(e.a,e.b);
  }
  const auto less=[](const auto &a,const auto &b) {
    return a.a<b.a || (a.a==b.a && a.b<b.b);
  };
  std::sort(edges.begin(),edges.end(),less);
  for (std::size_t k=1;k<edges.size();++k) {
    if (edges[k-1].a==edges[k].a && edges[k-1].b==edges[k].b)
      throw std::invalid_argument("Duplicate clay edge");
  }
  if (dab.amount==0 || weight_sum==0) return out;
  if (dab.action==ClayLayerAction::Deposit) {
    // Area-weighted budget: no increase in applied volume when tessellation
    // becomes denser. Empty or fully masked footprint consumes no material.
    for (std::size_t i=0;i<n;++i) {
      const double add=dab.amount*((weights[i]*cells[i].area)/weight_sum);
      out.volume[i]+=add;
      out.added_volume+=add;
    }
  }
  else {
    struct Flux {std::size_t from,to;double volume;};
    std::vector<Flux> flux;
    flux.reserve(edges.size());
    std::vector<double> outgoing(n,0.0);
    for (const auto &e : edges) {
      if (weights[e.a]==0 || weights[e.b]==0) continue;
      const auto &a=cells[e.a];const auto &b=cells[e.b];
      const Vec3 edge=b.position-a.position;
      const double len=norm(edge);
      if (!std::isfinite(len)) throw std::overflow_error("Clay edge overflow");
      if (len<=1e-12) continue;
      const Vec3 dir=edge/len;
      const double factor=dab.amount*dab.mobility*std::min(weights[e.a],weights[e.b]);
      double signed_flux=0.0;
      if (dab.action==ClayLayerAction::Spread) {
        // Pair equalization of thickness, not a global geometry smooth.
        const double lo=std::min(a.area,b.area),hi=std::max(a.area,b.area);
        const double effective_area=lo/(1.0+lo/hi);
        signed_flux=(height[e.a]-height[e.b])*effective_area*factor;
      }
      else {
        const Vec3 mid=(a.position+b.position)*0.5;
        const Vec3 field=dab.action==ClayLayerAction::Gather ?
                             unit(dab.center-mid) : unit(dab.direction);
        const double alignment=std::clamp(dot(field,dir),-1.0,1.0);
        const double donor=alignment>=0 ? a.volume : b.volume;
        signed_flux=donor*alignment*factor;
      }
      if (!std::isfinite(signed_flux)) throw std::overflow_error("Clay flux overflow");
      if (signed_flux==0) continue;
      const std::size_t from=signed_flux>0 ? e.a:e.b,to=signed_flux>0 ? e.b:e.a;
      const double v=std::abs(signed_flux);
      outgoing[from]+=v;
      flux.push_back({from,to,v});
    }
    // Simultaneous donor-limited flux: no negative inventory, no material
    // creation at the receiving front, independent of edge iteration order.
    std::vector<double> sent(n),received(n);
    for (const auto &f : flux) {
      if (!std::isfinite(outgoing[f.from])) throw std::overflow_error("Clay outgoing overflow");
      const double available=cells[f.from].volume*0.5;
      const double scale=outgoing[f.from]>available ? available/outgoing[f.from]:1.0;
      const double v=f.volume*scale;
      sent[f.from]+=v;received[f.to]+=v;out.transported_volume+=v;
    }
    for (std::size_t i=0;i<n;++i) {
      out.volume[i]=cells[i].volume-sent[i]+received[i];
    }
  }
  for (std::size_t i=0;i<n;++i) {
    const double delta=(out.volume[i]-cells[i].volume)/cells[i].area;
    if (!std::isfinite(out.volume[i]) || out.volume[i]<0 ||
        !std::isfinite(delta)) throw std::overflow_error("Invalid staged clay layer");
    out.displacement[i]=unit(cells[i].normal)*delta;
  }
  if (!std::isfinite(out.added_volume) || !std::isfinite(out.transported_volume))
    throw std::overflow_error("Invalid staged clay budget");
  return out;
}

} // namespace sculpt_physics
