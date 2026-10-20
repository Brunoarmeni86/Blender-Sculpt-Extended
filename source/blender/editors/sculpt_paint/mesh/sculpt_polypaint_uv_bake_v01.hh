/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BLI_string.h"
#include <vector>
#include "BKE_attribute.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "DNA_image_types.h"
#include "IMB_imbuf_types.hh"

/* UV_POLYPAINT_BAKE_V01. CPU, base-corner bilinear UV patches.
 * Reads master color directly, independent of displayed sculpt level.
 * Each invocation creates one image, never overwrites an existing image. */
namespace blender::ed::sculpt_paint {
namespace polypaint_uv_v01 {
static bool inverse_patch(const float2 &a, const float2 &b, const float2 &c,
                          const float2 &d, const float2 &p, float &u, float &v)
{
  u = v = 0.5f;
  for (int i = 0; i < 16; i++) {
    const float2 q = a * ((1-u)*(1-v)) + b*(u*(1-v)) + c*((1-u)*v) + d*(u*v);
    const float2 du = (b-a)*(1-v)+(d-c)*v;
    const float2 dv = (c-a)*(1-u)+(d-b)*u;
    const float det = du.x*dv.y-du.y*dv.x;
    if (std::abs(det) < 1e-20f) return false;
    const float2 e = q-p;
    const float su = (e.x*dv.y-e.y*dv.x)/det;
    const float sv = (du.x*e.y-du.y*e.x)/det;
    u -= su; v -= sv;
    if (!std::isfinite(u) || !std::isfinite(v)) return false;
    if (std::abs(su)+std::abs(sv) < 1e-6f) break;
  }
  const float2 q = a*((1-u)*(1-v))+b*(u*(1-v))+c*((1-u)*v)+d*(u*v);
  return u >= -2e-5f && u <= 1.00002f && v >= -2e-5f && v <= 1.00002f &&
         std::abs(q.x-p.x)+std::abs(q.y-p.y) < 2e-5f;
}

static wmOperatorStatus exec(bContext *C, wmOperator *op)
{
  Object *object = CTX_data_active_object(C);
  if (!object || object->type != OB_MESH || object->mode != OB_MODE_OBJECT) {
    BKE_report(op->reports, RPT_ERROR, "Select a mesh in Object Mode before baking Polypaint");
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *id_cast<Mesh *>(object->data);
  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object->modifiers) {
    if (md.type == eModifierType_Multires) {
      if (mmd) {
        BKE_report(op->reports, RPT_ERROR, "Use exactly one Multires modifier");
        return OPERATOR_CANCELLED;
      }
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
    }
  }
  if (!mmd) return OPERATOR_CANCELLED;
  char attribute[256], uv_name[256];
  RNA_string_get(op->ptr, "attribute", attribute);
  RNA_string_get(op->ptr, "uv_map", uv_name);
  if (!attribute[0]) {
    BLI_strncpy(attribute, mmd->sculpt_color_attribute, sizeof(attribute));
  }
  if (!attribute[0]) {
    BKE_report(op->reports, RPT_ERROR, "Enter the Polypaint Color Attribute name");
    return OPERATOR_CANCELLED;
  }
  if (!uv_name[0]) {
    const char *active_uv = CustomData_get_active_layer_name(&mesh.corner_data, CD_PROP_FLOAT2);
    if (active_uv) BLI_strncpy(uv_name, active_uv, sizeof(uv_name));
  }
  const bke::AttributeAccessor attrs = mesh.attributes();
  const VArray<float2> uv = *attrs.lookup<float2>(uv_name, bke::AttrDomain::Corner);
  if (!uv || uv.size() != mesh.corners_num) {
    BKE_report(op->reports, RPT_ERROR, "UV map not found; enter its exact name");
    return OPERATOR_CANCELLED;
  }
  for (int i = 0; i < mesh.corners_num; i++) {
    if (!std::isfinite(uv[i].x) || !std::isfinite(uv[i].y)) {
      BKE_report(op->reports, RPT_ERROR, "UV map contains non-finite coordinates");
      return OPERATOR_CANCELLED;
    }
  }
  const int size = RNA_int_get(op->ptr, "resolution");
  const int tile = RNA_int_get(op->ptr, "tile");
  const int tx = (tile-1001)%10, ty = (tile-1001)/10;
  /* POLYPAINT_UV_AA_V02: ownership tracked per subpixel, including patch seams. */
  const int samples = RNA_int_get(op->ptr, "samples");
  const int sample_count = samples * samples;
  const uint8_t full_mask = uint8_t((1u << sample_count) - 1u);
  const int pixels_num = size*size;
  Array<float4> pixels(pixels_num, float4(0.0f));
  Array<uint8_t> covered(pixels_num, uint8_t(0));
  int painted = 0;
  bool missing = false;
  const OffsetIndices<int> faces = mesh.faces();
  for (const int fi : faces.index_range()) {
    const IndexRange face = faces[fi];
    if (face.size() < 3) continue;
    float2 center(0.0f);
    for (int ci : face) center += uv[ci]/float(face.size());
    for (int local = 0; local < face.size(); local++) {
      const int ci = face.start()+local;
      const float2 a = center;
      const float2 b = (uv[ci]+uv[face.start()+(local+1)%face.size()])*0.5f;
      const float2 c = (uv[ci]+uv[face.start()+(local+face.size()-1)%face.size()])*0.5f;
      const float2 d = uv[ci];
      const float minx = std::min({a.x,b.x,c.x,d.x})-tx;
      const float maxx = std::max({a.x,b.x,c.x,d.x})-tx;
      const float miny = std::min({a.y,b.y,c.y,d.y})-ty;
      const float maxy = std::max({a.y,b.y,c.y,d.y})-ty;
      if (maxx < 0 || minx > 1 || maxy < 0 || miny > 1) continue;
      const int x0 = int(std::floor(std::clamp(minx,0.0f,1.0f)*size));
      const int x1 = std::min(size-1,int(std::ceil(std::clamp(maxx,0.0f,1.0f)*size)));
      const int y0 = int(std::floor(std::clamp(miny,0.0f,1.0f)*size));
      const int y1 = std::min(size-1,int(std::ceil(std::clamp(maxy,0.0f,1.0f)*size)));
      for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++) {
        const int index=y*size+x;
        if (covered[index] == full_mask) continue;
        for (int sy=0; sy<samples && !missing; sy++) {
          for (int sx=0; sx<samples; sx++) {
            const uint8_t bit = uint8_t(1u << (sy*samples+sx));
            if (covered[index] & bit) continue;
            float u,v,rgba[4];
            const float px = tx+(x+(sx+0.5f)/samples)/size;
            const float py = ty+(y+(sy+0.5f)/samples)/size;
            if (!inverse_patch(a,b,c,d,float2(px,py),u,v)) continue;
            u=std::clamp(u,0.0f,1.0f);v=std::clamp(v,0.0f,1.0f);
            if (!bke::multires_layers::polypaint_sample_with_base(*mmd,mesh,attribute,ci,u,v,rgba)) {
              missing=true;break;
            }
            for (int k=0;k<4;k++) if (!std::isfinite(rgba[k])) missing=true;
            if (missing) break;
            if (!covered[index]) painted++;
            /* Straight RGB output: accumulate premultiplied color to avoid
             * transparent colors bleeding into opaque samples. */
            const float alpha = std::clamp(rgba[3],0.0f,1.0f);
            for(int k=0;k<3;k++) pixels[index][k] += rgba[k]*alpha;
            pixels[index][3] += alpha;
            covered[index] |= bit;
          }
        }
      }
      if (missing) break;
    }
    if (missing) break;
  }
  if (missing || !painted) {
    BKE_report(op->reports,RPT_ERROR,missing ? "Color samples unavailable or non-finite; no image created" : "No UV coverage in selected tile");
    return OPERATOR_CANCELLED;
  }
  for (int i=0;i<pixels_num;i++) if (covered[i]) {
    const float alpha_sum = pixels[i][3];
    if (alpha_sum > 0.0f) {
      for(int k=0;k<3;k++) pixels[i][k] /= alpha_sum;
    }
    pixels[i][3] = alpha_sum / float(sample_count);
  }
  std::vector<int> frontier;
  for(int i=0;i<pixels_num;i++) if(covered[i]) {
    const int x=i%size,y=i/size;
    if ((x && !covered[i-1]) || (x+1<size && !covered[i+1]) ||
        (y && !covered[i-size]) || (y+1<size && !covered[i+size])) frontier.push_back(i);
  }
  for(int step=0;step<RNA_int_get(op->ptr,"margin") && !frontier.empty();step++) {
    std::vector<int> next;
    for(int i:frontier) {
      const int x=i%size,y=i/size;
      const int neighbors[4]={x?i-1:-1,x+1<size?i+1:-1,y?i-size:-1,y+1<size?i+size:-1};
      for(int j:neighbors) if(j>=0 && !covered[j]) {
        covered[j]=1;pixels[j]=pixels[i];next.push_back(j);
      }
    }
    frontier.swap(next);
  }
  char name[128];
  std::snprintf(name,sizeof(name),"PolypaintBake.%d",tile);
  const float clear[4]={0,0,0,0};
  Image *image=BKE_image_add_generated(CTX_data_main(C),size,size,name,32,1,IMA_GENTYPE_BLANK,clear,false,false,false);
  if(!image) return OPERATOR_CANCELLED;
  void *lock=nullptr;
  ImBuf *ibuf=BKE_image_acquire_ibuf(image,nullptr,&lock);
  if(!ibuf || !ibuf->float_buffer.data) {
    BKE_image_release_ibuf(image,ibuf,lock);
    BKE_id_free(CTX_data_main(C),image);
    return OPERATOR_CANCELLED;
  }
  float *image_pixels = ibuf->float_data_for_write();
  for(int i=0;i<pixels_num;i++) for(int k=0;k<4;k++) image_pixels[i*4+k]=pixels[i][k];
  ibuf->userflags |= IB_BITMAPDIRTY;
  BKE_image_release_ibuf(image,ibuf,lock);
  WM_event_add_notifier(C,NC_IMAGE|NA_ADDED,image);
  BKE_reportf(op->reports,RPT_INFO,"Created %s: %d covered pixels. Save from Image Editor",image->id.name+2,painted);
  return OPERATOR_FINISHED;
}
} // namespace blender::ed::sculpt_paint::polypaint_uv_v01

static wmOperatorStatus polypaint_uv_bake_v01_invoke(bContext *C, wmOperator *op, const wmEvent *)
{
  return WM_operator_props_dialog_popup(C, op, 420);
}

static void SCULPT_OT_multires_polypaint_uv_bake_v01(wmOperatorType *ot)
{
  ot->name="Bake Multires Polypaint to UV";
  ot->idname="SCULPT_OT_multires_polypaint_uv_bake_v01";
  ot->description="Bake high-resolution Polypaint master to one UV tile as an image";
  ot->exec=polypaint_uv_v01::exec;
  ot->invoke=polypaint_uv_bake_v01_invoke;
  ot->flag=OPTYPE_REGISTER|OPTYPE_UNDO;
  RNA_def_string(ot->srna,"attribute","",256,"Color Attribute","Empty uses the active Multires Polypaint attribute");
  RNA_def_string(ot->srna,"uv_map","",256,"UV Map","Empty uses active UV map; V01 uses bilinear cage UV patches");
  RNA_def_int(ot->srna,"resolution",2048,64,4096,"Resolution","Square image size",64,4096);
  RNA_def_int(ot->srna,"tile",1001,1001,2000,"UDIM Tile","One separate image per invocation",1001,1100);
  RNA_def_int(ot->srna,"samples",2,1,2,"Samples per Axis","1: center pixel; 2: four subpixel samples",1,2);
  RNA_def_int(ot->srna,"margin",8,0,64,"Margin","Pixel dilation within this tile",0,32);
}

} // namespace blender::ed::sculpt_paint
