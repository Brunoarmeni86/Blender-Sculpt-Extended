#include <cassert>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <string>
static std::unordered_map<void*,size_t> allocations;
template<class T> T *MEM_new(const char*) {T*p=new T{};allocations[p]=sizeof(T);return p;}
template<class T> T *MEM_new_array_zeroed(size_t n,const char*){T*p=static_cast<T*>(::operator new(sizeof(T)*n));std::memset(p,0,sizeof(T)*n);allocations[p]=sizeof(T)*n;return p;}
template<class T> void MEM_delete(T*p){if(p){assert(allocations.erase(p)==1);delete p;}}
template<class T> void del_array(T*p){if(p){assert(allocations.erase(p)==1);::operator delete(p);}}
#define MEM_SAFE_DELETE(p) do{del_array(p);p=nullptr;}while(0)
#define STREQ(a,b) (std::strcmp((a),(b))==0)
static void BLI_strncpy(char*d,const char*s,size_t n){std::strncpy(d,s,n-1);d[n-1]=0;}
template<class T> struct ListBaseT {
 T*first=nullptr,*last=nullptr;
 struct Iter{T*p;T&operator*()const{return *p;}Iter&operator++(){p=p->next;return *this;}bool operator!=(Iter b)const{return p!=b.p;}};
 Iter begin()const{return {first};}Iter end()const{return {nullptr};}
 void clear_no_delete(){first=last=nullptr;}
 bool is_empty()const{return first==nullptr;}
 int count()const{int n=0;for(auto&p:*this){(void)p;++n;}return n;}
};
template<class T>void BLI_remlink(ListBaseT<T>*l,T*p){if(p->prev)p->prev->next=p->next;else l->first=p->next;if(p->next)p->next->prev=p->prev;else l->last=p->prev;p->next=p->prev=nullptr;}
template<class T>void BLI_addtail(ListBaseT<T>*l,T*p){p->prev=l->last;p->next=nullptr;if(l->last)l->last->next=p;else l->first=p;l->last=p;}
struct MultiresColorGrid{float(*colors)[4]=nullptr;int totcolor=0;int level=0;};
struct MultiresColorAttribute{MultiresColorAttribute *next=nullptr,*prev=nullptr;char name[68]="";MultiresColorGrid*grids=nullptr;int grids_num=0,level=0;};
struct MultiresModifierData{MultiresColorGrid*sculpt_color_grids=nullptr;int sculpt_color_grids_num=0,sculpt_color_level=0;char sculpt_color_attribute[68]="";ListBaseT<MultiresColorAttribute> sculpt_color_attributes;int geometry_sentinel=679;};

template<class T>T *MEM_dupalloc(T*source){size_t n=allocations.at(source);T*p=static_cast<T*>(::operator new(n));std::memcpy(p,source,n);allocations[p]=n;return p;}
struct Serialized {
 std::unordered_map<const void*,std::vector<unsigned char>> bytes;
 std::vector<MultiresColorAttribute> entries;
 void write_raw(size_t n,const void*p){bytes[p]=std::vector<unsigned char>((const unsigned char*)p,(const unsigned char*)p+n);}
 template<class T>void write_struct_array(size_t n,const T*p){write_raw(sizeof(T)*n,p);}
 void write_struct_list(const ListBaseT<MultiresColorAttribute>*l){for(auto&a:*l)entries.push_back(a);}
};
using BlendDataReader=Serialized;
template<class T>bool BLO_read_array(BlendDataReader*r,T**p,int n,int components=1){
 if(!*p)return false;
 auto it=r->bytes.find(*p);if(it==r->bytes.end()||it->second.size()!=sizeof(T)*n*components)return false;
 T*copy=MEM_new_array_zeroed<T>(n*components,"deserialize");std::memcpy(copy,it->second.data(),it->second.size());*p=copy;return true;
}
static void read_bank_list(BlendDataReader*r,ListBaseT<MultiresColorAttribute>*l){
 l->clear_no_delete();for(auto&saved:r->entries){auto*p=MEM_new<MultiresColorAttribute>("read");*p=saved;BLI_addtail(l,p);}
}
#define BLO_read_struct_list(reader,Type,list) read_bank_list(reader,list)
void polypaint_clear(MultiresModifierData &mmd)
{
  if (mmd.sculpt_color_grids) {
    for (int i = 0; i < mmd.sculpt_color_grids_num; i++) {
      MEM_SAFE_DELETE(mmd.sculpt_color_grids[i].colors);
    }
    del_array(mmd.sculpt_color_grids);
  }
  mmd.sculpt_color_grids = nullptr;
  mmd.sculpt_color_grids_num = 0;
  mmd.sculpt_color_level = 0;
}

/* POLYPAINT_MULTI_ATTRIBUTE_V1: one active owner, inactive named owners.
 * Read views are borrowed and never alter the source or free its allocations. */
static bool polypaint_attribute_name_valid(const char *name)
{
  return name && name[0] && std::strlen(name) < sizeof(MultiresColorAttribute::name);
}

static MultiresColorAttribute *polypaint_attribute_find(MultiresModifierData &mmd,
                                                       const char *name)
{
  for (MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
    if (STREQ(attribute.name, name)) {
      return &attribute;
    }
  }
  return nullptr;
}

bool polypaint_attribute_view(const MultiresModifierData &mmd,
                             const char *name, MultiresModifierData &r_view)
{
  if (!polypaint_attribute_name_valid(name)) {
    return false;
  }
  /* A legacy file has no name yet. Its only master belongs to the current
   * mesh color attribute; callers must only use this fallback for that name. */
  if (STREQ(mmd.sculpt_color_attribute, name) ||
      (!mmd.sculpt_color_attribute[0] && mmd.sculpt_color_attributes.is_empty())) {
    r_view.sculpt_color_grids = mmd.sculpt_color_grids;
    r_view.sculpt_color_grids_num = mmd.sculpt_color_grids_num;
    r_view.sculpt_color_level = mmd.sculpt_color_level;
    return true;
  }
  for (const MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
    if (STREQ(attribute.name, name)) {
      r_view.sculpt_color_grids = attribute.grids;
      r_view.sculpt_color_grids_num = attribute.grids_num;
      r_view.sculpt_color_level = attribute.level;
      return true;
    }
  }
  return false;
}

bool polypaint_attribute_activate(MultiresModifierData &mmd, const char *name)
{
  if (!polypaint_attribute_name_valid(name)) {
    return false;
  }
  if (!mmd.sculpt_color_attribute[0]) {
    if (!mmd.sculpt_color_grids) {
      if (MultiresColorAttribute *target = polypaint_attribute_find(mmd, name)) {
        mmd.sculpt_color_grids = target->grids;
        mmd.sculpt_color_grids_num = target->grids_num;
        mmd.sculpt_color_level = target->level;
        BLI_remlink(&mmd.sculpt_color_attributes, target);
        MEM_delete(target);
      }
    }
    /* Migration only: the caller supplies the original mesh's current name
     * before selecting a different attribute. No old colors are reallocated. */
    BLI_strncpy(mmd.sculpt_color_attribute, name, sizeof(mmd.sculpt_color_attribute));
    return true;
  }
  if (STREQ(mmd.sculpt_color_attribute, name)) {
    return true;
  }
  MultiresColorAttribute *target = polypaint_attribute_find(mmd, name);
  /* Validate both directories before publishing the ownership exchange. */
  if ((mmd.sculpt_color_grids && (mmd.sculpt_color_level < 1 ||
                                 mmd.sculpt_color_level > 13 || mmd.sculpt_color_grids_num < 0)) ||
      (target && target->grids && (target->level < 1 || target->level > 13 ||
                                 target->grids_num < 0))) {
    return false;
  }
  auto *old = MEM_new<MultiresColorAttribute>("inactive Polypaint attribute");
  BLI_strncpy(old->name, mmd.sculpt_color_attribute, sizeof(old->name));
  old->grids = mmd.sculpt_color_grids;
  old->grids_num = mmd.sculpt_color_grids_num;
  old->level = mmd.sculpt_color_level;
  mmd.sculpt_color_grids = target ? target->grids : nullptr;
  mmd.sculpt_color_grids_num = target ? target->grids_num : 0;
  mmd.sculpt_color_level = target ? target->level : 0;
  BLI_strncpy(mmd.sculpt_color_attribute, name, sizeof(mmd.sculpt_color_attribute));
  if (target) {
    BLI_remlink(&mmd.sculpt_color_attributes, target);
    MEM_delete(target); /* Payload ownership has moved to the active slot. */
  }
  BLI_addtail(&mmd.sculpt_color_attributes, old);
  return true;
}

void polypaint_attribute_remove(MultiresModifierData &mmd, const char *name)
{
  if (!polypaint_attribute_name_valid(name)) {
    return;
  }
  if (STREQ(mmd.sculpt_color_attribute, name)) {
    polypaint_clear(mmd);
    mmd.sculpt_color_attribute[0] = '\0';
    return;
  }
  if (MultiresColorAttribute *attribute = polypaint_attribute_find(mmd, name)) {
    MultiresModifierData owned{};
    owned.sculpt_color_grids = attribute->grids;
    owned.sculpt_color_grids_num = attribute->grids_num;
    owned.sculpt_color_level = attribute->level;
    polypaint_clear(owned);
    BLI_remlink(&mmd.sculpt_color_attributes, attribute);
    MEM_delete(attribute);
  }
}

bool polypaint_attribute_rename(MultiresModifierData &mmd, const char *old_name,
                               const char *new_name)
{
  if (!polypaint_attribute_name_valid(old_name) || !polypaint_attribute_name_valid(new_name)) {
    return false;
  }
  if (STREQ(old_name, new_name)) {
    return true;
  }
  if (STREQ(mmd.sculpt_color_attribute, new_name) || polypaint_attribute_find(mmd, new_name)) {
    return false;
  }
  if (STREQ(mmd.sculpt_color_attribute, old_name)) {
    BLI_strncpy(mmd.sculpt_color_attribute, new_name, sizeof(mmd.sculpt_color_attribute));
  }
  else if (MultiresColorAttribute *attribute = polypaint_attribute_find(mmd, old_name)) {
    BLI_strncpy(attribute->name, new_name, sizeof(attribute->name));
  }
  return true;
}

void polypaint_attributes_clear(MultiresModifierData &mmd)
{
  while (!mmd.sculpt_color_attributes.is_empty()) {
    auto *attribute = static_cast<MultiresColorAttribute *>(mmd.sculpt_color_attributes.first);
    /* Do not use remove(name) here: malformed duplicate names must also free
     * the exact list entry, never the active allocation. */
    MultiresModifierData owned{};
    owned.sculpt_color_grids = attribute->grids;
    owned.sculpt_color_grids_num = attribute->grids_num;
    owned.sculpt_color_level = attribute->level;
    polypaint_clear(owned);
    BLI_remlink(&mmd.sculpt_color_attributes, attribute);
    MEM_delete(attribute);
  }
  polypaint_clear(mmd);
  mmd.sculpt_color_attribute[0] = '\0';
}

void copy_banks(MultiresModifierData &dst,const MultiresModifierData &src) {
  dst.sculpt_color_attributes.clear_no_delete();
  BLI_strncpy(dst.sculpt_color_attribute, src.sculpt_color_attribute,
              sizeof(dst.sculpt_color_attribute));
  for (const MultiresColorAttribute &source : src.sculpt_color_attributes) {
    auto *destination = MEM_new<MultiresColorAttribute>("Polypaint attribute copy");
    BLI_strncpy(destination->name, source.name, sizeof(destination->name));
    destination->grids_num = source.grids_num;
    destination->level = source.level;
    if (source.grids && source.grids_num > 0) {
      destination->grids = MEM_new_array_zeroed<MultiresColorGrid>(
          source.grids_num, "Polypaint attribute directory copy");
      for (int i = 0; i < source.grids_num; i++) {
        destination->grids[i] = source.grids[i];
        destination->grids[i].colors = source.grids[i].colors ?
            MEM_dupalloc(source.grids[i].colors) : nullptr;
      }
    }
    BLI_addtail(&dst.sculpt_color_attributes, destination);
  }

}
void write_banks(Serialized *writer,const MultiresModifierData *mmd) {
  writer->write_struct_list(&mmd->sculpt_color_attributes);
  for (const MultiresColorAttribute &attribute : mmd->sculpt_color_attributes) {
    if (attribute.grids && attribute.grids_num > 0) {
      writer->write_struct_array(attribute.grids_num, attribute.grids);
      for (int i = 0; i < attribute.grids_num; i++) {
        const MultiresColorGrid &grid = attribute.grids[i];
        if (grid.colors && grid.totcolor > 0) {
          writer->write_raw(sizeof(float) * size_t(grid.totcolor) * 4, grid.colors);
        }
      }
    }
  }

}
void read_banks(BlendDataReader *reader,MultiresModifierData *mmd) {
  BLO_read_struct_list(reader, MultiresColorAttribute, &mmd->sculpt_color_attributes);
  mmd->sculpt_color_attribute[sizeof(mmd->sculpt_color_attribute) - 1] = '\0';
  for (MultiresColorAttribute &attribute : mmd->sculpt_color_attributes) {
    attribute.name[sizeof(attribute.name) - 1] = '\0';
    if (attribute.grids_num <= 0 || !attribute.grids ||
        !BLO_read_array(reader, &attribute.grids, attribute.grids_num)) {
      attribute.grids = nullptr;
      attribute.grids_num = 0;
      attribute.level = 0;
      continue;
    }
    for (int i = 0; i < attribute.grids_num; i++) {
      MultiresColorGrid &grid = attribute.grids[i];
      if (grid.totcolor > 0 && grid.colors &&
          !BLO_read_array(reader, reinterpret_cast<float **>(&grid.colors), grid.totcolor, 4)) {
        grid.colors = nullptr;
        grid.totcolor = 0;
      }
    }
  }

}
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
