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
