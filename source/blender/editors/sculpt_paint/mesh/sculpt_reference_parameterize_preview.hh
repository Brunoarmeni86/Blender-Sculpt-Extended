/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Temporary orphan meshes only: positive-weight harmonic disk coordinates, O(V+E) storage. */
static wmOperatorStatus reference_harmonic_solve_exec(bContext *C, wmOperator *op)
{
  const char *enabled=std::getenv("BLENDER_SCULPT_SYMMETRY_REFERENCE_V01");
  if(!enabled || enabled[0]!='1' || enabled[1]!='\0')return OPERATOR_CANCELLED;
  char name[256];RNA_string_get(op->ptr,"mesh",name);
  auto *mesh=reinterpret_cast<Mesh *>(BKE_libblock_find_name(CTX_data_main(C),ID_ME,name));
  if(!mesh || mesh->id.us!=0 || mesh->id.lib || std::strncmp(name,"__symref_param_",15)!=0 ||
      mesh->verts_num>1000000 || mesh->faces_num!=0) {
    BKE_report(op->reports,RPT_ERROR,"Invalid temporary parameterization mesh");return OPERATOR_CANCELLED;
  }
  const bool vector_field=RNA_boolean_get(op->ptr,"vector_field");
  const int n=mesh->verts_num;
  const auto pin_attribute=mesh->attributes().lookup<bool>(".symref_pin",bke::AttrDomain::Point);
  if(!pin_attribute || n<3)return OPERATOR_CANCELLED;
  const VArray<bool> pins=pin_attribute.varray;
  const Span<float3> positions=mesh->vert_positions();
  const Span<int2> edges=mesh->edges();
  Array<int> offsets(n+1,0);
  for(const int2 &edge:edges){offsets[edge.x+1]++;offsets[edge.y+1]++;}
  for(int i=1;i<=n;i++)offsets[i]+=offsets[i-1];
  Array<int> cursor(offsets.as_span());Array<int> neighbours(edges.size()*2);
  for(const int2 &edge:edges){neighbours[cursor[edge.x]++]=edge.y;neighbours[cursor[edge.y]++]=edge.x;}
  Array<int> free_index(n,-1);Vector<int> free_vertices;int pinned_count=0;
  for(int i=0;i<n;i++){
    if(!std::isfinite(positions[i].x) || !std::isfinite(positions[i].y) || !std::isfinite(positions[i].z) || offsets[i+1]==offsets[i])return OPERATOR_CANCELLED;
    if(pins[i])pinned_count++;else {free_index[i]=free_vertices.size();free_vertices.append(i);}
  }
  if(pinned_count<3)return OPERATOR_CANCELLED;
  /* Reject components with no Dirichlet boundary instead of solving a singular system. */
  Array<bool> visited(n,false);Vector<int> stack;
  for(int start=0;start<n;start++){
    if(visited[start])continue;bool has_pin=false;stack.append(start);visited[start]=true;
    while(!stack.is_empty()){
      const int i=stack.pop_last();has_pin|=pins[i];
      for(int k=offsets[i];k<offsets[i+1];k++){const int j=neighbours[k];if(!visited[j]){visited[j]=true;stack.append(j);}}
    }
    if(!has_pin){BKE_report(op->reports,RPT_ERROR,"Unpinned harmonic component");return OPERATOR_CANCELLED;}
  }
  const int m=free_vertices.size();Array<float3> result(positions);
  Array<double> x(m),rhs(m),residual(m),z(m),direction(m),product(m),diagonal(m);
  for(int j=0;j<m;j++)diagonal[j]=double(offsets[free_vertices[j]+1]-offsets[free_vertices[j]]);
  const auto multiply=[&](const Span<double> input,const MutableSpan<double> output){
    for(int j=0;j<m;j++){
      const int i=free_vertices[j];double value=diagonal[j]*input[j];
      for(int k=offsets[i];k<offsets[i+1];k++){const int q=free_index[neighbours[k]];if(q>=0)value-=input[q];}
      output[j]=value;
    }
  };
  const auto dot=[](const Span<double> a,const Span<double> b){double sum=0;for(int i=0;i<a.size();i++)sum+=a[i]*b[i];return sum;};
  int maximum_iterations=0;
  for(int axis=0;axis<(vector_field?3:2);axis++){
    x.fill(0);rhs.fill(0);
    for(int j=0;j<m;j++){
      const int i=free_vertices[j];
      for(int k=offsets[i];k<offsets[i+1];k++){const int v=neighbours[k];if(free_index[v]<0)rhs[j]+=positions[v][axis];}
    }
    residual.as_mutable_span().copy_from(rhs);for(int j=0;j<m;j++)z[j]=residual[j]/diagonal[j];direction.as_mutable_span().copy_from(z);
    const double bnorm=dot(rhs,rhs);double rz=dot(residual,z);int iteration=0;
    const int limit=std::min(6000,std::max(400,6*int(std::sqrt(double(m)))));
    while(dot(residual,residual)>std::max(1e-28,bnorm*1e-18) && iteration<limit){
      multiply(direction,product);const double denominator=dot(direction,product);
      if(!std::isfinite(denominator) || denominator<=0){BKE_report(op->reports,RPT_ERROR,"Degenerate harmonic solve");return OPERATOR_CANCELLED;}
      const double alpha=rz/denominator;
      for(int j=0;j<m;j++){x[j]+=alpha*direction[j];residual[j]-=alpha*product[j];z[j]=residual[j]/diagonal[j];}
      const double next=dot(residual,z),beta=next/rz;rz=next;
      for(int j=0;j<m;j++)direction[j]=z[j]+beta*direction[j];iteration++;
    }
    multiply(x,product);double error=0;for(int j=0;j<m;j++){const double d=rhs[j]-product[j];error+=d*d;}
    if(!std::isfinite(error) || error>std::max(1e-24,bnorm*1e-14)){
      BKE_report(op->reports,RPT_ERROR,"Harmonic parameterization did not converge; no shape changes applied");return OPERATOR_CANCELLED;
    }
    for(int j=0;j<m;j++){if(!std::isfinite(x[j]))return OPERATOR_CANCELLED;result[free_vertices[j]][axis]=float(x[j]);if(!vector_field)result[free_vertices[j]].z=0;}
    maximum_iterations=std::max(maximum_iterations,iteration);
  }
  if(!vector_field)for(const float3 &p:result){if(p.x*p.x+p.y*p.y>1.0001f)return OPERATOR_CANCELLED;}
  mesh->vert_positions_for_write().copy_from(result);mesh->tag_positions_changed();
  std::printf("[SYMREF_HARMONIC] verts=%d free=%d iterations=%d\n",n,m,maximum_iterations);
  return OPERATOR_FINISHED;
}
static void SCULPT_OT_reference_harmonic_solve_preview(wmOperatorType *ot)
{
  ot->name="Reference Harmonic Parameterization";ot->idname="SCULPT_OT_reference_harmonic_solve_preview";
  ot->exec=reference_harmonic_solve_exec;ot->flag=OPTYPE_INTERNAL;
  RNA_def_boolean(ot->srna,"vector_field",false,"Vector field","Solve a masked displacement correction instead of disk coordinates");
  RNA_def_string(ot->srna,"mesh",nullptr,256,"Temporary mesh","Unlinked temporary disk mesh only");
}
