#pragma once
#include <map>
#include <string>
inline std::map<std::string,std::string> env;
inline const char *BLI_getenv(const char *k){auto i=env.find(k);return i==env.end()?nullptr:i->second.c_str();}
inline void BLI_setenv_if_new(const char*k,const char*v){env.emplace(k,v);}
