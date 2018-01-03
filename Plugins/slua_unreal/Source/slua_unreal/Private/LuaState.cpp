// The MIT License (MIT)

// Copyright 2015 Siney/Pangweiwei siney@yeah.net
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


#include "LuaState.h"
#include "lua/lua.hpp"
#include "LuaObject.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "Blueprint/UserWidget.h"
#include "Misc/AssertionMacros.h"
#include "Log.h"
#include "Util.h"
#include <map>

namespace slua {

    static UWorld* world = nullptr;
    std::map<lua_State*,LuaState*> stateMap;

    int import(lua_State *L) {
        const char* name = LuaObject::checkValue<const char*>(L,1);
        if(name) {
            UClass* uclass = FindObject<UClass>(ANY_PACKAGE, UTF8_TO_TCHAR(name));
            if(uclass) {
                LuaObject::pushClass(L,uclass);
                return 1;
            }
            UScriptStruct* ustruct = FindObject<UScriptStruct>(ANY_PACKAGE, UTF8_TO_TCHAR(name));
            if(ustruct) {
                LuaObject::pushStruct(L,ustruct);
                return 1;
            }

            
            luaL_error(L,"Can't find class named %s",name);
        }
        return 0;
    }
    
    int print(lua_State *L) {
        std::string str;
        int top = lua_gettop(L);
        for(int n=1;n<=top;n++) {
            size_t len;
            const char* s = luaL_tolstring(L, n, &len);
            str+="\t";
            if(s)
                str+=s;
        }
        Log::Log("%s",str.c_str());
        return 0;
    }

    int error(lua_State* L) {
        const char* err = lua_tostring(L,1);
        Log::Error("%s",err);
        return 0;
    }

    int loadUI(lua_State* L) {
        const char* ui = luaL_checkstring(L,1);

        TArray<FStringFormatArg> Args;
		Args.Add(UTF8_TO_TCHAR(ui));

        // load blueprint widget from cpp, need add '_C' tail
        auto cui = FString::Format(TEXT("Blueprint'{0}_C'"),Args);
        TSubclassOf<UUserWidget> uclass = LoadClass<UUserWidget>(NULL, *cui);
        if(uclass==nullptr)
            luaL_error(L,"Can't find ui named %s",ui);

        UUserWidget* widget = CreateWidget<UUserWidget>(world,uclass);
        return LuaObject::push(L,widget);
    }

    int LuaState::loader(lua_State* L) {
        LuaState* state = LuaState::get(L);
        const char* fn = lua_tostring(L,1);
        uint32 len;
        if(uint8* buf = state->loadFile(fn,len)) {
            AutoDeleteArray<uint8> defer(buf);

            char chunk[256];
            snprintf(chunk,256,"@%s",fn);
            if(luaL_loadbuffer(L,(const char*)buf,len,chunk)==0) {
                return 1;
            }
            else {
                const char* err = lua_tostring(L,-1);
                Log::Error("%s",err);
                lua_pop(L,1);
            }
        }
        else
            Log::Error("Can't load file %s",fn);
        return 0;
    }
    
    uint8* LuaState::loadFile(const char* fn,uint32& len) {
        if(loadFileDelegate) return loadFileDelegate(fn,len);
        return nullptr;
    }

    

    LuaState::LuaState()
        :loadFileDelegate(nullptr)
        ,L(nullptr)
    {
    }

    LuaState::~LuaState()
    {
        close();
    }

    LuaState* LuaState::get(lua_State* L) {
        auto it = stateMap.find(L);
        if(it!=stateMap.end())
            return it->second;
        else
            return nullptr;
    }

    void LuaState::close() {
        if(L) {
            stateMap.erase(L);
            lua_close(L);
            L=nullptr;
        }
        world=nullptr;
    }


    bool LuaState::init(UWorld* wld) {

        if(!wld)
            return false;

        world = wld;

        L = luaL_newstate();
        stateMap[L]=this;
        
        luaL_openlibs(L);
        
        lua_pushcfunction(L,import);
        lua_setglobal(L, "import");
        
        lua_pushcfunction(L,print);
        lua_setglobal(L, "print");

        lua_pushcfunction(L,loadUI);
        lua_setglobal(L, "loadUI");

        lua_pushcfunction(L,loader);
        int loaderFunc = lua_gettop(L);

        lua_getglobal(L,"package");
        lua_getfield(L,-1,"searchers");

        int loaderTable = lua_gettop(L);

        for(int i=lua_rawlen(L,loaderTable)+1;i>2;i--) {
            lua_rawgeti(L,loaderTable,i-1);
            lua_rawseti(L,loaderTable,i);
        }
        lua_pushvalue(L,loaderFunc);
        lua_rawseti(L,loaderTable,2);

        LuaObject::init(L);

        lua_settop(L,0);

        return true;
    }

    LuaVar LuaState::doBuffer(const uint8* buf,uint32 len, const char* chunk) {
        AutoStack g(L);
        int errfunc = pushErrorHandler(L);

        if(luaL_loadbuffer(L, (const char *)buf, len, chunk)) {
            const char* err = lua_tostring(L,-1);
            Log::Error("DoBuffer failed: %s",err);
            return LuaVar();
        }
        
        if(lua_pcall(L, 0, LUA_MULTRET, errfunc)) {
            return LuaVar();
        }
        return LuaVar();
    }

    LuaVar LuaState::doString(const char* str) {
        return doBuffer((const uint8*)str,strlen(str),str);
    }

    LuaVar LuaState::doFile(const char* fn) {
        uint32 len;
        if(uint8* buf=loadFile(fn,len)) {
            char chunk[256];
            snprintf(chunk,256,"@%s",fn);

            const LuaVar& r = doBuffer( buf,len,chunk );
            delete[] buf;
            return r;
        }
        return LuaVar();
    }

    int LuaState::pushErrorHandler(lua_State* L) {
        auto it = stateMap.find(L);
        ensure(it!=stateMap.end());

        auto ls = it->second;
        return ls->_pushErrorHandler(L);
    }

    int LuaState::_pushErrorHandler(lua_State* state) {
        lua_pushcfunction(state,error);
        return lua_gettop(state);
    }

}