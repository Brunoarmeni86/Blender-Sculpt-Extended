void populate(MultiresModifierData&m,int level){
 m.totlvl=7;m.sculptlvl=level;assert(polypaint_ensure(m,level,3));
 auto&g=m.sculpt_color_grids[0];int side=subdiv::grid_size_from_level(level);
 g.level=level;g.totcolor=side*side;g.colors=MEM_new_array_uninitialized<float[4]>(g.totcolor,"fixture");
 for(int i=0;i<g.totcolor;i++)for(int c=0;c<4;c++)g.colors[i][c]=float((i*71+c*23)%997)/996.f;
 g.colors[0][0]=-0.f;
}
void reset_undo(){undo_stack={};undo_steps={};undo_steps[0].type=&memfile_type;undo_stack.step_active=&undo_steps[0];pushes=fail_push=tags=notifiers=0;compatible=true;U={};snapshots.clear();}
int main(){
 setenv("BLENDER_SCULPT_POLYPAINT_LAZY_MASTER","1",1);int cases=0;
 for(int old=1;old<=5;old++)for(int target=old+1;target<=old+3;target++){
  MultiresModifierData m,staged;populate(m,old);auto*source=m.sculpt_color_grids[0].colors;
  int oldside=subdiv::grid_size_from_level(old),oldarea=oldside*oldside;
  std::vector<float>before(reinterpret_cast<float*>(source),reinterpret_cast<float*>(source)+oldarea*4);
  assert(polypaint_stage_master(m,target,3,staged));
  assert(m.sculpt_color_level==old && m.sculpt_color_grids[0].colors==source);
  assert(!std::memcmp(source,before.data(),before.size()*sizeof(float)));
  int newside=subdiv::grid_size_from_level(target),stride=(newside-1)/(oldside-1);
  for(int y=0;y<oldside;y++)for(int x=0;x<oldside;x++)assert(!std::memcmp(staged.sculpt_color_grids[0].colors[(y*stride)*newside+x*stride],source[y*oldside+x],sizeof(float[4])));
  for(int i=0;i<newside*newside;i++)for(int c=0;c<4;c++)assert(std::isfinite(staged.sculpt_color_grids[0].colors[i][c]));
  assert(!staged.sculpt_color_grids[1].colors&&!staged.sculpt_color_grids[2].colors);
  polypaint_swap_master(m,staged);assert(m.sculpt_color_level==target);assert(m.shape_layers_sentinel==123&&m.recording_sentinel==456&&m.totlvl==7);
  polypaint_swap_master(m,staged);assert(m.sculpt_color_grids[0].colors==source);assert(!std::memcmp(source,before.data(),before.size()*sizeof(float)));
  polypaint_clear(staged);polypaint_clear(m);assert(allocations.empty());cases++;
 }
 for(int bad=0;bad<6;bad++){
  MultiresModifierData m,staged;populate(m,2);
  if(bad==0)m.sculpt_color_grids[0].level=1;
  if(bad==1)m.sculpt_color_grids[0].totcolor--;
  if(bad==2)m.sculpt_color_grids_num=2;
  if(bad==3)m.sculpt_color_grids[0].colors[4][0]=std::numeric_limits<float>::quiet_NaN();
  int target=bad==4?1:bad==5?14:3;
  assert(!polypaint_stage_master(m,target,3,staged));assert(!staged.sculpt_color_grids);m.sculpt_color_grids_num=3;
  polypaint_clear(m);assert(allocations.empty());cases++;
 }
 for(int scenario=0;scenario<9;scenario++){
  reset_undo();MultiresModifierData m;populate(m,2);m.sculptlvl=3;m.totlvl=6;
  Mesh mesh;SculptSession ss;Runtime runtime{&ss};Object ob{&mesh,&runtime,&m};Scene scene;bContext context{&ob,&scene};wmOperator op;
  auto*original=m.sculpt_color_grids[0].colors;
  if(scenario==1)m.sculptlvl=1; // Lower-level Paint with finer geometry: no promotion.
  if(scenario==2)fail_push=1;
  if(scenario==3)fail_push=2;
  if(scenario==4)undo_stack.step_init=&undo_steps[0];
  if(scenario==5)compatible=false;
  if(scenario==6)ss.cache=&ss;
  if(scenario==7)U.uiflag=0;
  if(scenario==8)context.paint=false;
  bool success=sculpt_polypaint_prepare_stroke_master(&context,&op);
  bool expected=scenario==0||scenario==1||scenario==7||scenario==8;assert(success==expected);
  if(scenario==0){assert(m.sculpt_color_level==3);assert(snapshots==std::vector<int>({2,3}));}
  else if(scenario==7){assert(m.sculpt_color_level==3&&pushes==0);}
  else{assert(m.sculpt_color_level==2&&m.sculpt_color_grids[0].colors==original);}
  assert(m.totlvl==6&&m.shape_layers_sentinel==123&&m.recording_sentinel==456);
  polypaint_clear(m);assert(allocations.empty());cases++;
 }
 reset_undo();MultiresModifierData m;m.totlvl=7;m.sculptlvl=2;Mesh mesh;Runtime rt;Object ob{&mesh,&rt,&m};Scene scene;bContext ctx{&ob,&scene};wmOperator op;
 assert(sculpt_polypaint_prepare_stroke_master(&ctx,&op));assert(m.sculpt_color_level==2&&m.totlvl==7);assert(snapshots==std::vector<int>({0,2}));
 for(int i=0;i<3;i++)assert(!m.sculpt_color_grids[i].colors);polypaint_clear(m);assert(allocations.empty());cases++;
 std::cout<<"PASS "<<cases<<" actual staging/editor cases: retained bits, sparse/implicit colors, rollback, late promotion, checkpoint failures, geometry/layer isolation\n";
 std::cout<<"LIMIT: scene, ED_undo_push and allocator stubbed; native Undo/Redo must pass Windows tests\n";
}
