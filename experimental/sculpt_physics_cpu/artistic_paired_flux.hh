/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {
// Nonnegative directed volume proposals. CPU prepares geometry, donor and
// receiver capacities; a backend performs simultaneous limiting and exchange.
struct PairedFluxLink { std::size_t a,b; double amount; };
struct PairedFluxBackend {
 virtual ~PairedFluxBackend()=default;
 virtual bool exchange(const std::vector<PairedFluxLink>& links,
     const std::vector<double>& outgoing_cap,const std::vector<double>& incoming_cap,
     std::vector<double>& change,double& transferred)=0;
};
inline void paired_flux_reference(const std::vector<PairedFluxLink>& links,
 const std::vector<double>& outgoing_cap,const std::vector<double>& incoming_cap,
 std::vector<double>& change,double& transferred){
 const auto n=outgoing_cap.size();
 if(incoming_cap.size()!=n)throw std::invalid_argument("paired flux capacities");
 std::vector<double> outgoing(n),incoming(n);
 for(std::size_t i=0;i<n;++i)
  if(!std::isfinite(outgoing_cap[i])||outgoing_cap[i]<0||
     !std::isfinite(incoming_cap[i])||incoming_cap[i]<0)
   throw std::invalid_argument("paired flux capacity");
 for(auto f:links){
  if(f.a>=n||f.b>=n||f.a==f.b||!std::isfinite(f.amount)||f.amount<0)
   throw std::invalid_argument("paired flux link");
  outgoing[f.a]+=f.amount;incoming[f.b]+=f.amount;
 }
 change.assign(n,0.);transferred=0.;
 for(auto f:links)if(f.amount>0){
  const double mass=f.amount*std::min({1.,outgoing_cap[f.a]/outgoing[f.a],
                                      incoming_cap[f.b]/incoming[f.b]});
  change[f.a]-=mass;change[f.b]+=mass;transferred+=mass;
 }
}
// A GPU result never reaches geometry if its basic conservation, capacity
// or finite checks fail. CPU fallback replaces every output.
inline bool paired_flux_dispatch(PairedFluxBackend* backend,
 const std::vector<PairedFluxLink>& links,const std::vector<double>& outgoing_cap,
 const std::vector<double>& incoming_cap,std::vector<double>& change,double& transferred){
 bool ok=false;
 if(backend && !links.empty() && outgoing_cap.size()==incoming_cap.size()){
  try{
   ok=backend->exchange(links,outgoing_cap,incoming_cap,change,transferred);
   ok=ok&&change.size()==outgoing_cap.size()&&std::isfinite(transferred)&&transferred>=0;
   double balance=0.,magnitude=0.,proposal=0.;
   for(const auto& f:links)proposal+=f.amount;
   const double tol=1e-5*std::max(1e-30,proposal);
   if(ok)for(std::size_t i=0;i<change.size();++i){
    const double x=change[i];
    if(!std::isfinite(x)||x < -outgoing_cap[i]-tol||x > incoming_cap[i]+tol||
       ((outgoing_cap[i]==0&&incoming_cap[i]==0)&&x!=0.)){ok=false;break;}
    balance+=x;magnitude+=std::abs(x);
   }
   ok=ok&&std::abs(balance)<=tol&&magnitude<=2.*transferred+tol&&transferred<=proposal+tol;
  }catch(const std::exception&){ok=false;}
 }
 if(!ok)paired_flux_reference(links,outgoing_cap,incoming_cap,change,transferred);
 return ok;
}
}
