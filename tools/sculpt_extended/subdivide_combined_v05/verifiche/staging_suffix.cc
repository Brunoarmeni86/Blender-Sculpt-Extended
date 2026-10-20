void release(std::vector<MDisps> &a) { for(auto &g:a) { delete[] g.disps; delete[] g.hidden; } }
using Stage = bool (*)(const MultiresSculptLayer&, const MDisps*, MDisps*, int, int, bool, Span<int8_t>);
void trial(Stage stage, bool sparse, bool invalid_base, bool invalid_old) {
  const int count=8, old_side=3, side=5;
  std::vector<MDisps> base(count), old(count), expected(count), actual(count);
  std::vector<int8_t> support(count,1); support[1]=support[4]=support[6]=0;
  for(int g=0;g<count;++g) {
    old[g].level=2; old[g].totdisp=old_side*old_side;
    if(!sparse || support[g]) old[g].disps=new float[old_side*old_side][3]();
    if(g==4 || g==3) { old[g].hidden=new bool[old_side*old_side](); old[g].hidden[4]=true; }
    for(auto *v:{&base,&expected,&actual}) {
      (*v)[g].level=3; (*v)[g].totdisp=side*side;
      (*v)[g].disps=new float[side*side][3]();
      (*v)[g].hidden=new bool[side*side](); (*v)[g].hidden[0]=true;
    }
    for(int i=0;i<side*side;++i) for(int a=0;a<3;++a) {
      base[g].disps[i][a]=float((g+1)*17+i*3+a)/31.0f;
      expected[g].disps[i][a]=base[g].disps[i][a]+(support[g] ? .125f : 0.0f);
      actual[g].disps[i][a]=support[g] ? expected[g].disps[i][a] : 12345.0f;
    }
  }
  if(invalid_base) base[1].disps[3][1]=std::numeric_limits<float>::infinity();
  if(invalid_old) {
    if(!old[1].disps) old[1].disps=new float[old_side*old_side][3]();
    old[1].disps[3][1]=std::numeric_limits<float>::quiet_NaN();
  }
  MultiresSculptLayer layer{old.data(),count,2};
  const bool wanted=stage(layer,base.data(),expected.data(),count,3,true,{});
  const bool got=stage(layer,base.data(),actual.data(),count,3,true,support);
  assert(wanted==got);
  if(got) for(int g=0;g<count;++g) {
    assert(actual[g].level==expected[g].level && actual[g].totdisp==expected[g].totdisp);
    assert(bool(actual[g].hidden)==bool(expected[g].hidden));
    if(actual[g].hidden) for(int i=0;i<side*side;++i) assert(actual[g].hidden[i]==expected[g].hidden[i]);
    if(support[g]) assert(std::memcmp(actual[g].disps,expected[g].disps,side*side*sizeof(float[3]))==0);
    else {
      assert(!actual[g].disps);
      for(int i=0;i<side*side;++i) for(int a=0;a<3;++a) assert(expected[g].disps[i][a]==0.0f);
    }
  }
  release(base); release(old); release(expected); release(actual);
}
int main() {
  for(Stage stage:{multires_stage_subdivided_layer_serial_v01,multires_stage_subdivided_layer})
    for(bool sparse:{false,true}) {
      trial(stage,sparse,false,false); trial(stage,sparse,true,false); trial(stage,sparse,false,true);
    }
  std::cout << "PASS actual V05 serial/parallel staging: V04-equivalent deltas, implicit zeros, sparse inputs, hidden mapping, nonfinite base and old-layer rejection\n";
}
