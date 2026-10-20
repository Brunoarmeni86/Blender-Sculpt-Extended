/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "sculpt_polypaint_v04_api.hh"
#include "sculpt_undo.hh"
#include "sculpt_polypaint_local_v04.hh"
#include "sculpt_polypaint_smear_v03_math.hh"
#include "BLI_bitmap.h"
#include "BKE_customdata.hh"
#include "DNA_meshdata_types.h"
#include <set>
#include "sculpt_polypaint_v04_wet.hh"
#include "sculpt_polypaint_v04_project.hh"
namespace blender::ed::sculpt_paint::color {
static uint64_t color_v04_wet_samples=0,color_v04_wet_fallbacks=0,color_v04_wet_reuses=0;
static uint64_t color_v04_dabs=0,color_v04_aborts=0,color_v04_paint_dabs=0,color_v04_blur_dabs=0,color_v04_smear_dabs=0;
inline MultiresModifierData *color_v04_modifier(Object &ob){
 MultiresModifierData *result=nullptr;
 for(ModifierData &md:ob.modifiers)if(md.type==eModifierType_Multires){if(result)return nullptr;result=reinterpret_cast<MultiresModifierData*>(&md);}
 return result;
}
static void color_v04_abort(const char *reason){color_v04_aborts++;std::printf("[POLYPAINT_V04] abort=%s published=0\n",reason);std::fflush(stdout);}
using V04RGBA=color_local_v04::RGBA;
struct ColorV04GridStage {
 int index=0,side=0;
 float (*colors)[4]=nullptr;
 ~ColorV04GridStage(){MEM_SAFE_DELETE(colors);}
};
static void do_polypaint_all_v04(const Depsgraph &depsgraph,const Sculpt &sd,Object &ob,const IndexMask &node_mask){
 auto &ss=*ob.runtime->sculpt_session;auto *mmd=color_v04_modifier(ob);
 if(!polypaint_v04_enabled()||!ss.cache||ss.filter_cache||!mmd)return;
 auto &mesh=*id_cast<Mesh*>(ob.data);auto &tree=*bke::object::pbvh_get(ob);
 const bool cage=tree.type()==bke::pbvh::Type::Mesh&&mmd->sculptlvl==0;
 if(!cage&&tree.type()!=bke::pbvh::Type::Grids)return;
 auto *ccg=ss.subdiv_ccg;
 if(!cage&&!ccg)return;
 const char *name=mesh.active_color_attribute;
 const int master_level=mmd->sculpt_color_level;
 if(!name||!name[0]||!mmd->sculpt_color_grids||mmd->sculpt_color_grids_num!=mesh.corners_num||
    master_level<std::max(1,int(mmd->sculptlvl))||master_level>13||std::strcmp(name,mmd->sculpt_color_attribute)!=0)return;
 color_v04_dabs++;
 const int low_side=cage?2:ccg->grid_size,low_area=low_side*low_side;
 const int high_side=bke::subdiv::grid_size_from_level(master_level),high_area=high_side*high_side;
 if(high_side<low_side||(high_side-1)%(low_side-1))return;
 const auto &brush=*BKE_paint_brush_for_read(&sd.paint);
 const bool paint=brush.sculpt_brush_type==SCULPT_BRUSH_TYPE_PAINT;
 const bool smear=brush.sculpt_brush_type==SCULPT_BRUSH_TYPE_SMEAR;
 if(smear&&stroke_is_first_brush_step_of_symmetry_pass(*ss.cache))return;
 if(paint)color_v04_paint_dabs++;else if(smear)color_v04_smear_dabs++;else color_v04_blur_dabs++;
 const size_t budget=color_v03_work_bytes(),limit=budget/640;
 const float strength=std::clamp(std::abs(ss.cache->bstrength),0.f,1.f);
 if(!std::isfinite(strength)||strength<=0)return;
 if(paint&&(!std::isfinite(ss.cache->paint_brush.flow)||ss.cache->paint_brush.flow<=0||BKE_brush_alpha_get(&sd.paint,&brush)<=0))return;
 if(!cage&&(ccg->grids_num!=mesh.corners_num||ccg->positions.size()!=int64_t(ccg->grids_num)*low_area||ccg->normals.size()!=ccg->positions.size()))return;
 for(int g=0;g<mesh.corners_num;g++){const auto &v=mmd->sculpt_color_grids[g];if(v.colors&&(v.level!=master_level||v.totcolor!=high_area)){color_v04_abort("invalid_grid");return;}}
 std::unordered_map<int,float> factors_by_index;
 Vector<float3> co_buffer;Vector<float> factors,distances;
 bool refused=false;
 const Span<float3> mesh_positions=cage?bke::pbvh::vert_positions_eval(depsgraph,ob):Span<float3>{};
 const Span<float3> mesh_normals=cage?bke::pbvh::vert_normals_eval(depsgraph,ob):Span<float3>{};
 const auto faces=mesh.faces();const auto corner_verts=mesh.corner_verts();const auto vert_faces=mesh.vert_to_face_map();
 const MeshAttributeData mesh_attributes(mesh);
 const auto *hidden_grids=static_cast<const MDisps*>(CustomData_get_layer(&mesh.corner_data,CD_MDISPS));
 const auto *mask_grids=static_cast<const GridPaintMask*>(CustomData_get_layer(&mesh.corner_data,CD_GRID_PAINT_MASK));

 auto position=[&](int i)->float3{return cage?mesh_positions[i]:ccg->positions[i];};
 auto normal=[&](int i)->float3{return cage?mesh_normals[i]:ccg->normals[i];};
 const float density=std::clamp(ss.cache->paint_brush.density,0.f,1.f);
 auto collect=[&](int i,float factor){
  float f=std::clamp(factor*strength,0.f,1.f);
  if(cage&&f>0){
   float high_mask=0;bool high_hidden=false;
   for(int face:vert_faces[i])for(int c:faces[face])if(corner_verts[c]==i){
    if(mask_grids&&mask_grids[c].data){
     if(mask_grids[c].level<1||mask_grids[c].level>13){refused=true;return;}
     const int side=bke::subdiv::grid_size_from_level(mask_grids[c].level);
     high_mask=std::max(high_mask,mask_grids[c].data[side*side-1]);
    }
    if(hidden_grids&&hidden_grids[c].hidden){
     if(hidden_grids[c].level<1||hidden_grids[c].level>13){refused=true;return;}
     const int side=bke::subdiv::grid_size_from_level(hidden_grids[c].level);
     high_hidden|=bool(BLI_BITMAP_TEST(hidden_grids[c].hidden,side*side-1));
    }
   }
   const float base_mask=mesh_attributes.mask.is_empty()?0.f:mesh_attributes.mask[i];
   f=wet_v04c::cage_factor(f,base_mask,high_mask,high_hidden);
  }
  if(paint){
   if(density<=0)f=0;
   else if(density<1){
    const auto p=position(i);uint32_t bits[3];std::memcpy(bits,&p.x,sizeof(bits));const uint32_t h=bits[0]*73856093u^bits[1]*19349663u^bits[2]*83492791u;
    const float noise=BLI_hash_int_01(h);if(noise>density)f*=density*noise;
   }
  }
  if(std::isfinite(f)&&f>0){if(factors_by_index.size()>=limit){refused=true;return;}factors_by_index.emplace(i,f);}
 };
 if(cage){
  const auto nodes=tree.nodes<bke::pbvh::MeshNode>();
  node_mask.foreach_index([&](int n){
   if(refused)return;
   calc_factors_common_mesh_indexed(depsgraph,brush,ob,mesh_attributes,mesh_positions,mesh_normals,nodes[n],factors,distances);
   const auto verts=nodes[n].verts();for(int j=0;j<verts.size();j++)collect(verts[j],factors[j]);
  });

 }else{
  const auto nodes=tree.nodes<bke::pbvh::GridsNode>();
  node_mask.foreach_index([&](int n){
   if(refused)return;const auto grids=nodes[n].grids();
   if(uint64_t(grids.size())*low_area>budget/64){refused=true;return;}
   const auto co=gather_grids_positions(*ccg,grids,co_buffer);
   calc_factors_common_grids(depsgraph,brush,ob,co,nodes[n],factors,distances);
   for(int j=0;j<co.size();j++)collect(grids[j/low_area]*low_area+j%low_area,factors[j]);
  });
 }
 if(refused){color_v04_abort("local_budget");return;}
 if(factors_by_index.empty())return;
 const CCGKey key=cage?CCGKey{}:BKE_subdiv_ccg_key_top_level(*ccg);
 auto hidden=[&](int i){return cage?(!mesh_attributes.hide_vert.is_empty()&&mesh_attributes.hide_vert[i]):(!ccg->grid_hidden.is_empty()&&ccg->grid_hidden[i/low_area][i%low_area]);};
 auto sample=[&](int i,V04RGBA &rgba){
  if(!cage){auto c=SubdivCCGCoord::from_index(key,i);return bke::multires_layers::polypaint_sample_with_base(*mmd,mesh,name,c.grid_index,float(c.x)/(low_side-1),float(c.y)/(low_side-1),rgba.data());}
  rgba={0,0,0,0};int count=0;
  for(int face:vert_faces[i])for(int c:faces[face])if(corner_verts[c]==i){float v[4];if(!bke::multires_layers::polypaint_sample_with_base(*mmd,mesh,name,c,1,1,v))return false;for(int k=0;k<4;k++)rgba[k]+=v[k];count++;}
  if(!count)return false;for(float &v:rgba)v/=float(count);return true;
 };
 auto duplicates=[&](int i){std::vector<int> out;if(cage)return out;const auto c=SubdivCCGCoord::from_index(key,i);
  if(c.x==0||c.y==0||c.x==low_side-1||c.y==low_side-1){SubdivCCGNeighbors adj;BKE_subdiv_ccg_neighbor_coords_get(*ccg,c,true,adj);for(const auto &v:adj.duplicates())out.push_back(v.to_index(key));}return out;};
 auto neighbors=[&](int i){std::vector<int> out;if(cage){Vector<int> ns;for(int v:vert_neighbors_get_mesh(faces,corner_verts,vert_faces,mesh_attributes.hide_poly,i,ns))out.push_back(v);}else{SubdivCCGNeighbors adj;BKE_subdiv_ccg_neighbor_coords_get(*ccg,SubdivCCGCoord::from_index(key,i),true,adj);for(const auto &v:adj.unique())out.push_back(v.to_index(key));}return out;};
 const float3 drag=brush.stroke_method==BRUSH_STROKE_ANCHORED?ss.cache->grab_delta_symm:ss.cache->location_symm-ss.cache->last_location_symm;
 auto weight=[&](int i,int j){if(!smear)return 1.;float3 motion=drag;
  if(brush.smear_deform_type==BRUSH_SMEAR_DEFORM_PINCH)motion=ss.cache->location_symm-position(i);
  else if(brush.smear_deform_type==BRUSH_SMEAR_DEFORM_EXPAND)motion=position(i)-ss.cache->location_symm;
  if(ss.cache->toggle_settings.invert)motion=-motion;const auto edge=position(j)-position(i),no=normal(i);
  return smear_v03_math::weight({motion.x,motion.y,motion.z},{edge.x,edge.y,edge.z},{no.x,no.y,no.z});};
 float4 wet_next=ss.cache->paint_brush.wet_mix_prev_color;
 float3 primary=ss.cache->toggle_settings.invert?BKE_brush_secondary_color_get(&sd.paint,&brush):BKE_brush_color_get(&sd.paint,&brush);
 const float wet_mix=std::clamp(ss.cache->paint_brush.wet_mix,0.f,1.f);
 if(paint&&wet_mix>0){
  wet_v04c::Sample wet_sample;
  const double radius=double(ss.cache->radius)*std::max(0.f,brush.wet_paint_radius_factor);
  const auto distance=[&](int i){return double(math::length_squared(position(i)-ss.cache->location_symm));};
  if(!wet_v04c::sample(factors_by_index,distance,hidden,sample,radius,wet_sample)){
   color_v04_abort("invalid_wet_source");return;
  }
  if(wet_sample.count){
   const V04RGBA previous={wet_next.x,wet_next.y,wet_next.z,wet_next.w};
   V04RGBA next;std::array<float,3> mixed;
   const float persistence=ss.cache->paint_brush.wet_persistence;
   if(!wet_v04c::mix(wet_sample,previous,ss.cache->first_time,persistence,wet_mix,
        {primary.x,primary.y,primary.z},next,mixed))return;
   for(int c=0;c<4;c++)wet_next[c]=next[c];
   for(int c=0;c<3;c++)primary[c]=mixed[c];
   color_v04_wet_samples+=wet_sample.count;
   color_v04_wet_fallbacks+=wet_sample.nearest_fallback;
   color_v04_wet_reuses+=!ss.cache->first_time&&previous[3]!=0&&persistence>0;
   std::printf("[POLYPAINT_V04_WET] samples=%llu fallback=%d persistence=%.6g history=%d radius=%.6g\n",
     (unsigned long long)wet_sample.count,int(wet_sample.nearest_fallback),double(persistence),
     int(!ss.cache->first_time&&previous[3]!=0),radius);std::fflush(stdout);
  }
 }

 std::function<bool(int,const V04RGBA&,float,V04RGBA&)> transform;
 if(paint)transform=[&](int,const V04RGBA &before,float factor,V04RGBA &after){
  const float alpha=std::clamp(factor*BKE_brush_alpha_get(&sd.paint,&brush)*ss.cache->paint_brush.flow,0.f,1.f);
  const float source[4]={primary.x*alpha,primary.y*alpha,primary.z*alpha,alpha};
  IMB_blend_color_float(after.data(),before.data(),source,IMB_BlendMode(brush.blend));
  for(float &value:after)value=std::clamp(value,0.f,1.f);
  return true;
 };
 color_local_v04::Result result;
 if(!color_local_v04::calculate(factors_by_index,sample,duplicates,neighbors,hidden,result,weight,smear,limit,transform)){color_v04_abort("source_or_budget");return;}
 if(result.writes.empty())return;
 std::map<int,std::map<int,V04RGBA>> delta;
 if(!cage){
  for(const auto &entry:result.writes){V04RGBA d;const auto &before=result.source.at(entry.first);for(int c=0;c<4;c++)d[c]=entry.second[c]-before[c];delta[entry.first/low_area][entry.first%low_area]=d;}
 }else{
  std::set<int> affected;for(const auto &entry:result.writes)for(int f:vert_faces[entry.first])affected.insert(f);
  auto vertex_delta=[&](int v){V04RGBA d{};const auto it=result.writes.find(v);if(it!=result.writes.end())for(int c=0;c<4;c++)d[c]=it->second[c]-result.source.at(v)[c];return d;};
  for(int f:affected){const auto face=faces[f];V04RGBA center{};for(int c:face){auto d=vertex_delta(corner_verts[c]);for(int k=0;k<4;k++)center[k]+=d[k]/float(face.size());}
   for(int j=0;j<face.size();j++){const int c=face[j];auto a=vertex_delta(corner_verts[c]),b=vertex_delta(corner_verts[face[(j+1)%face.size()]]),d=vertex_delta(corner_verts[face[(j+face.size()-1)%face.size()]]);V04RGBA next,prev;for(int k=0;k<4;k++){next[k]=(a[k]+b[k])*.5f;prev[k]=(a[k]+d[k])*.5f;}delta[c]={{0,center},{1,next},{2,prev},{3,a}};}
  }
 }
 std::map<int,std::unique_ptr<ColorV04GridStage>> staged;
 for(const auto &entry:delta){
  const int g=entry.first;
  if(uint64_t(result.source.size())*640+uint64_t(staged.size()+1)*high_area*sizeof(float[4])>budget){color_v04_abort("stage_budget");return;}
  auto stage=std::make_unique<ColorV04GridStage>();stage->index=g;stage->side=high_side;
  stage->colors=MEM_new_array_uninitialized<float[4]>(high_area,"Polypaint V04 staged master");
  for(int y=0;y<high_side;y++)for(int x=0;x<high_side;x++){
   const int i=y*high_side+x;const float u=float(x)/(high_side-1),v=float(y)/(high_side-1);
   float before[4];
   const auto &original=mmd->sculpt_color_grids[g];
   if(original.colors)std::memcpy(before,original.colors[i],sizeof(before));
   else if(!bke::multires_layers::polypaint_sample_with_base(*mmd,mesh,name,g,u,v,before))return;
   for(int c=0;c<4;c++)stage->colors[i][c]=before[c];
   double protection=0;
   if(hidden_grids&&hidden_grids[g].hidden&&hidden_grids[g].level>=1&&hidden_grids[g].level<=13){const int side=bke::subdiv::grid_size_from_level(hidden_grids[g].level);const int hx=int(std::round(u*(side-1))),hy=int(std::round(v*(side-1)));if(BLI_BITMAP_TEST(hidden_grids[g].hidden,hy*side+hx))protection=1;}
   if(high_side>low_side&&mask_grids&&mask_grids[g].data&&mask_grids[g].level>=1&&mask_grids[g].level<=13){const int side=bke::subdiv::grid_size_from_level(mask_grids[g].level);const int mx=int(std::round(u*(side-1))),my=int(std::round(v*(side-1)));protection=std::max(protection,double(std::clamp(mask_grids[g].data[my*side+mx],0.f,1.f)));}
   if(!color_project_v04::sample(before,high_side,low_side,x,y,entry.second,
        protection,paint?4:3,stage->colors[i]))return;
  }
  staged.emplace(g,std::move(stage));
 }
 if(cage){
  /* Corner colors at a vertex span neighboring PBVH nodes. Capture every
   * affected face before changing the base attribute, even outside the brush
   * nodes. Do not silently prune boundary vertices to fit an Undo footprint. */
  std::set<int> changed_faces;
  for(const auto &entry:result.writes)for(int f:vert_faces[entry.first])changed_faces.insert(f);
  IndexMaskMemory memory;
  const auto all_nodes=bke::pbvh::all_leaf_nodes(tree,memory);
  const auto nodes=tree.nodes<bke::pbvh::MeshNode>();
  const auto support_nodes=IndexMask::from_predicate(all_nodes,memory,[&](int n){
   for(int f:nodes[n].faces())if(changed_faces.find(f)!=changed_faces.end())return true;
   return false;
  },exec_mode::grain_size(1));
  undo::push_nodes(depsgraph,ob,support_nodes,undo::Type::Color);
  std::vector<int> grids;for(const auto &entry:staged)grids.push_back(entry.first);
  if(!undo::capture_color_master_v04(ob,grids.data(),int(grids.size()))){color_v04_abort("L0_undo_capture");return;}
  auto attribute=active_color_attribute_for_write(mesh);if(!attribute)return;
  for(const auto &entry:result.writes){const auto &v=entry.second;color_vert_set(faces,corner_verts,vert_faces,attribute.domain,entry.first,float4(v[0],v[1],v[2],v[3]),attribute.span);}
  attribute.finish();
  tree.tag_attribute_changed(support_nodes,name);
 }
 for(auto &entry:staged){auto &live=mmd->sculpt_color_grids[entry.first];MEM_SAFE_DELETE(live.colors);live.colors=entry.second->colors;entry.second->colors=nullptr;live.level=master_level;live.totcolor=high_area;}
 if(paint&&wet_mix>0)ss.cache->paint_brush.wet_mix_prev_color=wet_next;
 tree.tag_attribute_changed(node_mask,name);
 std::printf("[POLYPAINT_V04] level=%d master=%d brush=%d grids=%llu changed=%llu\n",int(mmd->sculptlvl),master_level,int(brush.sculpt_brush_type),(unsigned long long)staged.size(),(unsigned long long)result.writes.size());std::fflush(stdout);
}
}

#include "sculpt_polypaint_v04_probe.hh"
