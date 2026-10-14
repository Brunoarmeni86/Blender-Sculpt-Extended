static void materialize(MultiresModifierData&m,int level,float v){
 assert(!m.sculpt_color_grids);m.sculpt_color_level=level;m.sculpt_color_grids_num=3;
 m.sculpt_color_grids=MEM_new_array_zeroed<MultiresColorGrid>(3,"test");
 m.sculpt_color_grids[0].colors=MEM_new_array_zeroed<float[4]>(4,"test");
 m.sculpt_color_grids[0].totcolor=4;m.sculpt_color_grids[0].level=level;
 m.sculpt_color_grids[0].colors[2][1]=v;
}
int main(){
 int cases=0;
 for(int initial=1;initial<=8;initial++){
 MultiresModifierData m;materialize(m,initial,0.37f);auto*a=m.sculpt_color_grids;
 assert(polypaint_attribute_activate(m,"SkinColor"));assert(m.sculpt_color_grids==a);cases++;
 assert(polypaint_attribute_activate(m,"Mask"));assert(!m.sculpt_color_grids);assert(m.sculpt_color_attributes.count()==1);cases++;
 materialize(m,initial+1,0.81f);auto*b=m.sculpt_color_grids;
 MultiresModifierData view;assert(polypaint_attribute_view(m,"SkinColor",view));assert(view.sculpt_color_grids==a);assert(view.sculpt_color_grids[0].colors[2][1]==0.37f);cases++;
 assert(polypaint_attribute_activate(m,"SkinColor"));assert(m.sculpt_color_grids==a);assert(m.sculpt_color_level==initial);cases++;
 for(int t=0;t<30;t++){assert(polypaint_attribute_activate(m,"Mask"));assert(m.sculpt_color_grids==b);assert(polypaint_attribute_activate(m,"SkinColor"));assert(m.sculpt_color_grids==a);}assert(m.sculpt_color_attributes.count()==1);cases++;
 assert(polypaint_attribute_rename(m,"Mask","Roughness"));assert(!polypaint_attribute_view(m,"Mask",view));assert(polypaint_attribute_view(m,"Roughness",view));assert(view.sculpt_color_grids==b);cases++;
 assert(!polypaint_attribute_rename(m,"SkinColor","Roughness"));assert(!polypaint_attribute_activate(m,""));assert(!polypaint_attribute_activate(m,"this-name-is-over-sixty-eight-characters-and-must-be-rejected-without-any-mutation"));assert(m.sculpt_color_grids==a);cases++;
 assert(polypaint_attribute_rename(m,"SkinColor","Albedo"));assert(STREQ(m.sculpt_color_attribute,"Albedo"));cases++;
 polypaint_attribute_remove(m,"Albedo");assert(!m.sculpt_color_grids&&!m.sculpt_color_attribute[0]);assert(polypaint_attribute_activate(m,"Roughness"));assert(m.sculpt_color_grids==b);cases++;
 assert(polypaint_attribute_activate(m,"Empty"));assert(!m.sculpt_color_grids);polypaint_attribute_remove(m,"Roughness");assert(!polypaint_attribute_view(m,"Roughness",view));assert(m.geometry_sentinel==679);cases++;
 polypaint_attributes_clear(m);assert(m.sculpt_color_attributes.is_empty()&&!m.sculpt_color_grids&&allocations.empty());cases++;
 }
 {MultiresModifierData m;assert(polypaint_attribute_activate(m,"A"));assert(polypaint_attribute_activate(m,"B"));assert(polypaint_attribute_activate(m,"C"));assert(m.sculpt_color_attributes.count()==2);polypaint_attributes_clear(m);assert(allocations.empty());cases++;}
 {
 MultiresModifierData source;assert(polypaint_attribute_activate(source,"SkinColor"));materialize(source,2,.2f);
 assert(polypaint_attribute_activate(source,"Mask"));materialize(source,4,.8f);
 assert(polypaint_attribute_activate(source,"Roughness")); // Two inactive painted banks, active implicit.
 MultiresModifierData copy;copy_banks(copy,source);
 MultiresModifierData a,b;assert(polypaint_attribute_view(source,"Mask",a));assert(polypaint_attribute_view(copy,"Mask",b));
 assert(a.sculpt_color_grids!=b.sculpt_color_grids);assert(a.sculpt_color_grids[0].colors!=b.sculpt_color_grids[0].colors);
 b.sculpt_color_grids[0].colors[2][1]=.4f;assert(a.sculpt_color_grids[0].colors[2][1]==.8f);cases++;
 Serialized saved;write_banks(&saved,&source);std::string active=source.sculpt_color_attribute;
 polypaint_attributes_clear(source);polypaint_attributes_clear(copy);
 MultiresModifierData loaded;BLI_strncpy(loaded.sculpt_color_attribute,active.c_str(),sizeof(loaded.sculpt_color_attribute));read_banks(&saved,&loaded);
 assert(loaded.sculpt_color_attributes.count()==2);assert(polypaint_attribute_view(loaded,"SkinColor",a));assert(a.sculpt_color_level==2&&a.sculpt_color_grids[0].colors[2][1]==.2f);cases++;
 assert(polypaint_attribute_view(loaded,"Mask",a));assert(a.sculpt_color_level==4&&a.sculpt_color_grids[0].colors[2][1]==.8f);cases++;
 assert(!a.sculpt_color_grids[1].colors);assert(polypaint_attribute_activate(loaded,"Mask"));assert(loaded.sculpt_color_level==4);cases++;
 polypaint_attributes_clear(loaded);assert(allocations.empty());cases++;
 }
 std::printf("PASS %d actual ownership cases; allocator/ListBase modeled; full Blender persistence and Undo tested by Windows runner\n",cases);
}
