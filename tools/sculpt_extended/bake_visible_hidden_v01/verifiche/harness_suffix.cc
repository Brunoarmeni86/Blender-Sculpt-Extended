float (*delta(float x))[3]{auto d=new float[1][3]{};d[0][0]=x;return d;}
struct Fixture{MultiresModifierData m;Mesh mesh;MDisps base;Fixture(bool hr){m.totlvl=hr?1:0;mesh.corner_data.grids=&base;base.disps=delta(1);m.sculpt_layers.resize(3);for(int i=0;i<3;i++){auto&l=m.sculpt_layers[i];l.disps=new MDisps[1];l.disps[0].disps=delta(i+2);l.base_deltas=hr?nullptr:delta(i+2);l.base_verts_num=hr?0:1;}m.sculpt_layers[0].strength=.5;m.sculpt_layers[1].flag=0;m.sculpt_layers[1].strength=-.4;m.sculpt_layers[2].group_flag=2;m.sculpt_layer_active=1;}~Fixture(){delete[]base.disps;for(auto&l:m.sculpt_layers){delete[]l.base_deltas;delete[]l.disps[0].disps;delete[]l.disps;}}};
int main(){
 for(bool hr:{false,true}){
  {Fixture f(hr);auto*h=f.m.sculpt_layers[1].base_deltas;auto*g=f.m.sculpt_layers[2].base_deltas;auto*hd=f.m.sculpt_layers[1].disps[0].disps;assert(bake_visible_to_base(f.m,f.mesh));assert((hr?f.base.disps[0][0]:f.mesh.p[0][0])==2);assert(f.m.sculpt_layers[1].base_deltas==h&&f.m.sculpt_layers[2].base_deltas==g);assert(f.m.sculpt_layers[1].disps[0].disps==hd&&hd[0][0]==3);assert(f.m.sculpt_layer_active==1);assert(bake_visible_to_base(f.m,f.mesh));assert((hr?f.base.disps[0][0]:f.mesh.p[0][0])==2);f.m.sculpt_layers[0].flag=0;assert(!bake_visible_to_base(f.m,f.mesh));}
  {Fixture f(hr);auto&l=f.m.sculpt_layers[0];(hr?l.disps[0].disps:l.base_deltas)[0][0]=NAN;assert(!bake_visible_to_base(f.m,f.mesh));assert((hr?f.base.disps[0][0]:f.mesh.p[0][0])==1);assert((hr?l.disps[0].disps:l.base_deltas)!=nullptr);}
  {Fixture f(hr);auto&h=f.m.sculpt_layers[1];(hr?h.disps[0].disps:h.base_deltas)[0][0]=NAN;assert(bake_visible_to_base(f.m,f.mesh));assert(std::isnan((hr?h.disps[0].disps:h.base_deltas)[0][0]));}
  {Fixture f(hr);f.mesh.valid=false;assert(!bake_visible_to_base(f.m,f.mesh));}
  {Fixture f(hr);f.mesh.id.users=2;assert(!bake_visible_to_base(f.m,f.mesh));}
 }
 {Fixture f(true);f.m.sculpt_layers[0].base_deltas=delta(1);f.m.sculpt_layers[0].base_verts_num=1;assert(!bake_visible_to_base(f.m,f.mesh));assert(f.base.disps[0][0]==1);}
 std::cout<<"PASS actual bake/merge bodies: HR/L0, hidden and hidden-group preservation, all-hidden cancellation, repeat bake, invalid/nonfinite rejection, hidden nonfinite isolation, HR L0-rebase guard\n";
}
