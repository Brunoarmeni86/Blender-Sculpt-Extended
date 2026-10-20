/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
namespace blender::ed::sculpt_paint::color::color_project_v04 {
/* Interpolate immutable DELTAS once. Keep the original high-frequency residual.
 * Caller stages the complete output before publishing any live grid. */
template<class Delta>
bool sample(const float before[4],int high_side,int low_side,int x,int y,
            const Delta &delta,double protection,int channels,float out[4])
{
 if(low_side<2||high_side<low_side||(high_side-1)%(low_side-1)||
    x<0||y<0||x>=high_side||y>=high_side||channels<3||channels>4||
    !std::isfinite(protection)||protection<0||protection>1)return false;
 const int ratio=(high_side-1)/(low_side-1);
 const int lx=std::min(x/ratio,low_side-2),ly=std::min(y/ratio,low_side-2);
 const double a=double(x-lx*ratio)/ratio,b=double(y-ly*ratio)/ratio;
 const int indices[4]={ly*low_side+lx,ly*low_side+lx+1,
                       (ly+1)*low_side+lx,(ly+1)*low_side+lx+1};
 const double weights[4]={(1-a)*(1-b),a*(1-b),(1-a)*b,a*b};
 for(int c=0;c<4;c++){
  if(!std::isfinite(before[c]))return false;
  out[c]=before[c];
  if(c>=channels||protection==1)continue;
  double change=0;
  for(int k=0;k<4;k++){
   auto it=delta.find(indices[k]);
   if(it!=delta.end())change+=double(it->second[c])*weights[k];
  }
  if(!std::isfinite(change))return false;
  /* Preserve exact protected and zero-delta bit patterns, including -0. */
  if(change!=0)out[c]=float(double(before[c])+change*(1-protection));
  if(!std::isfinite(out[c]))return false;
 }
 return true;
}
}
