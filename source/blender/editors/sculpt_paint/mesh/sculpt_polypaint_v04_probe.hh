/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "BKE_multires_polypaint_l0_v04d.hh"
namespace blender::ed::sculpt_paint::color {
inline bool color_v04_export(bContext *C,wmOperator *op){
 Object &ob=*CTX_data_active_object(C);const auto &ss=*ob.runtime->sculpt_session;
 auto *mmd=color_v04_modifier(ob);if(!mmd)return false;
 const auto &mesh=*id_cast<const Mesh*>(ob.data);const bool summary=RNA_enum_get(op->ptr,"action")==4;
 const int level=std::max(1,int(mmd->sculpt_color_level?mmd->sculpt_color_level:mmd->totlvl));
 if(level<1||level>13||(mmd->sculpt_color_grids&&mmd->sculpt_color_grids_num!=mesh.corners_num))return false;
 const int side=bke::subdiv::grid_size_from_level(level),area=side*side;
 const int64_t total=int64_t(mesh.corners_num)*area;if(total>INT_MAX||(!summary&&total>1048576))return false;
 char path[FILE_MAX];RNA_string_get(op->ptr,"filepath",path);FILE *out=std::fopen(path,"wb");if(!out)return false;
 uint64_t colors_hash=1469598103934665603ULL,positions_hash=colors_hash,mask_hash=colors_hash,face_hash=colors_hash,layer_hash=colors_hash,base_color_hash=colors_hash,protection_hash=colors_hash;
 auto hash=[](uint64_t &h,const void *bytes,size_t size){const auto *b=static_cast<const unsigned char*>(bytes);for(size_t i=0;i<size;i++){h^=b[i];h*=1099511628211ULL;}};
 std::fprintf(out,"{\"wet_samples\":%llu,\"wet_fallbacks\":%llu,\"wet_reuses\":%llu,",(unsigned long long)color_v04_wet_samples,(unsigned long long)color_v04_wet_fallbacks,(unsigned long long)color_v04_wet_reuses);
 std::fprintf(out,"\"blur_backend\":4,\"blur_dabs\":%llu,\"smear_dabs\":%llu,\"paint_dabs\":%llu,\"budget_aborts\":%llu,\"level\":%d,\"master_level\":%d,\"master_side\":%d,\"present\":[",(unsigned long long)color_v04_blur_dabs,(unsigned long long)color_v04_smear_dabs,(unsigned long long)color_v04_paint_dabs,(unsigned long long)color_v04_aborts,int(mmd->sculptlvl),int(mmd->sculpt_color_level),side);
 for(int g=0;g<mesh.corners_num;g++)std::fprintf(out,"%s%d",g?",":"",int(mmd->sculpt_color_grids&&mmd->sculpt_color_grids[g].colors));
 std::fprintf(out,"],\"colors\":[");float first[4]={};bool nonuniform=false;
 const bool has_color=mesh.active_color_attribute&&mesh.active_color_attribute[0];
 for(int g=0;has_color&&g<mesh.corners_num;g++)for(int j=0;j<area;j++){
  float v[4];if(!bke::multires_layers::polypaint_sample_with_base(*mmd,mesh,mesh.active_color_attribute,g,float(j%side)/(side-1),float(j/side)/(side-1),v)){std::fclose(out);return false;}
  for(float c:v)if(!std::isfinite(c)){std::fclose(out);return false;}
  hash(colors_hash,v,sizeof(v));if(g==0&&j==0)std::memcpy(first,v,sizeof(v));else nonuniform|=std::memcmp(first,v,sizeof(v))!=0;
  if(!summary)std::fprintf(out,"%s[%.9g,%.9g,%.9g,%.9g]",g||j?",":"",v[0],v[1],v[2],v[3]);
 }
 std::fprintf(out,"],\"l0_preview\":[");
 if(has_color&&!summary){
  MultiresModifierData implicit{};
  for(int corner=0;corner<mesh.corners_num;corner++){
   float fallback[4],value[4];
   if(!bke::multires_layers::polypaint_sample_with_base(implicit,mesh,mesh.active_color_attribute,corner,.5f,.5f,fallback)){std::fclose(out);return false;}
   const auto *grid=mmd->sculpt_color_grids?&mmd->sculpt_color_grids[corner]:nullptr;
   bke::multires_polypaint_l0_v04d::restrict_grid(grid?grid->colors:nullptr,grid?grid->level:0,grid?grid->totcolor:0,fallback,value);
   std::fprintf(out,"%s[%.9g,%.9g,%.9g,%.9g]",corner?",":"",value[0],value[1],value[2],value[3]);
  }
 }
 std::fprintf(out,"],\"positions\":[");
 const auto positions=ss.subdiv_ccg?ss.subdiv_ccg->positions.as_span():mesh.vert_positions();
 for(int i=0;i<positions.size();i++){const auto &v=positions[i];hash(positions_hash,&v.x,sizeof(float)*3);if(!summary)std::fprintf(out,"%s[%.9g,%.9g,%.9g]",i?",":"",v.x,v.y,v.z);}
 std::fprintf(out,"],\"hidden\":[");bool first_hidden=true;
 if(ss.subdiv_ccg&&!ss.subdiv_ccg->grid_hidden.is_empty())for(int g=0;g<ss.subdiv_ccg->grids_num;g++)for(int j=0;j<ss.subdiv_ccg->grid_area;j++)if(ss.subdiv_ccg->grid_hidden[g][j]){std::fprintf(out,"%s%d",first_hidden?"":",",g*ss.subdiv_ccg->grid_area+j);first_hidden=false;}
 if(!ss.subdiv_ccg){const auto hide=*mesh.attributes().lookup_or_default<bool>(".hide_vert",bke::AttrDomain::Point,false);for(int i=0;i<mesh.verts_num;i++)if(hide[i]){std::fprintf(out,"%s%d",first_hidden?"":",",i);first_hidden=false;}}
 const auto ids=*mesh.attributes().lookup_or_default<int>(".sculpt_face_set",bke::AttrDomain::Face,1);
 for(int f=0;f<mesh.faces_num;f++){int id=ids[f];hash(face_hash,&id,sizeof(id));}
 if(bke::multires_layers::faceset_master_valid(*mmd,mesh.corners_num)){const int fs=bke::subdiv::grid_size_from_level(mmd->sculpt_face_set_level);for(int g=0;g<mesh.corners_num;g++)for(int j=0;j<fs*fs;j++){const auto &grid=mmd->sculpt_face_set_grids[g];int id=grid.ids?grid.ids[j]:grid.uniform_id;hash(face_hash,&id,sizeof(id));}}
 for(const MultiresSculptLayer &layer:mmd->sculpt_layers){hash(layer_hash,layer.name,sizeof(layer.name));hash(layer_hash,&layer.strength,sizeof(layer.strength));if(layer.base_deltas&&layer.base_verts_num>0)hash(layer_hash,layer.base_deltas,size_t(layer.base_verts_num)*sizeof(float[3]));if(layer.disps)for(int g=0;g<layer.grids_num;g++)if(layer.disps[g].disps&&layer.disps[g].totdisp>0)hash(layer_hash,layer.disps[g].disps,size_t(layer.disps[g].totdisp)*sizeof(float[3]));}
 const auto *hidden=static_cast<const MDisps*>(CustomData_get_layer(&mesh.corner_data,CD_MDISPS));const auto *masks=static_cast<const GridPaintMask*>(CustomData_get_layer(&mesh.corner_data,CD_GRID_PAINT_MASK));
 int full_mask_corners=0;
 for(int g=0;g<mesh.corners_num;g++){
  if(masks&&masks[g].data&&masks[g].level>=1&&masks[g].level<=13){const int ms=bke::subdiv::grid_size_from_level(masks[g].level);full_mask_corners+=masks[g].data[ms*ms-1]>=1.f;}
  if(hidden&&hidden[g].hidden){const int hs=bke::subdiv::grid_size_from_level(hidden[g].level);for(int j=0;j<hs*hs;j++){const unsigned char bit=BLI_BITMAP_TEST(hidden[g].hidden,j)?1:0;hash(protection_hash,&bit,1);}}
  if(masks&&masks[g].data){const int ms=bke::subdiv::grid_size_from_level(masks[g].level);hash(protection_hash,masks[g].data,size_t(ms)*ms*sizeof(float));}
 }
 if(has_color){const auto base=*mesh.attributes().lookup<ColorGeometry4f>(mesh.active_color_attribute,bke::AttrDomain::Corner);for(int c=0;c<base.size();c++){const auto v=base[c];hash(base_color_hash,&v.r,sizeof(float)*4);}}
 std::fprintf(out,"],\"hr_mask_full_corners\":%d,",full_mask_corners);
 std::fprintf(out,"\"face_sets_hash\":\"%llu\",\"layer_hash\":\"%llu\",\"protection_hash\":\"%llu\",\"base_color_hash\":\"%llu\",\"mask\":[",(unsigned long long)face_hash,(unsigned long long)layer_hash,(unsigned long long)protection_hash,(unsigned long long)base_color_hash);
 if(!ss.subdiv_ccg){const auto mask=*mesh.attributes().lookup_or_default<float>(".sculpt_mask",bke::AttrDomain::Point,0.f);for(int i=0;i<mesh.verts_num;i++){const float v=mask[i];hash(mask_hash,&v,sizeof(v));if(!summary)std::fprintf(out,"%s%.9g",i?",":"",v);}}
 if(ss.subdiv_ccg)for(int i=0;i<ss.subdiv_ccg->masks.size();i++){const float v=ss.subdiv_ccg->masks[i];hash(mask_hash,&v,sizeof(v));if(!summary)std::fprintf(out,"%s%.9g",i?",":"",v);}
 std::fprintf(out,"],\"sample_count\":%lld,\"colors_hash\":\"%llu\",\"positions_hash\":\"%llu\",\"mask_hash\":\"%llu\",\"nonuniform\":%s}\n",(long long)total,(unsigned long long)colors_hash,(unsigned long long)positions_hash,(unsigned long long)mask_hash,nonuniform?"true":"false");
 bool failed=std::ferror(out);int closed=std::fclose(out);return !failed&&!closed;
}
}
