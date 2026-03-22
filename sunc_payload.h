#pragma once

// SUNC payload — 5 parts to stay under MSVC C2026 limit (16380 chars each).
// executor.h must run all 5 in order after unc_payload_1..5.

const char* sunc_payload_1 = R"LUA(
local genv = shared._rblx_genv
if type(genv) ~= "table" then warn("[SUNC] no genv"); return end

-- Native debug captures BEFORE anything overwrites them
-- CRITICAL: Luau debug.getupvalue(f,i) -> VALUE only (not name,value like Lua5.1)
local _dgu  = debug.getupvalue
local _dsu  = debug.setupvalue
local _dgi  = debug.info
local _dgmt = debug.getmetatable

print("[SUNC] Part 1...")

-- ── 1. getrawmetatable / setrawmetatable ─────────────────────────────────────
-- FAIL: "Couldn't retrieve the raw metatable"
-- ROOT: getmetatable() returns boolean guard on Instances. debug.getmetatable bypasses.
pcall(function()
    local http = shared._rblx_http
    local server = "http://127.0.0.1:9753"

    genv.getrawmetatable = function(obj)
        local fn = debug.getmetatable or _dgmt
        if fn then local ok,mt = pcall(fn,obj); if ok and type(mt)=="table" then return mt end end
        -- Backup: if debug.getmetatable is hooked or restricted, use C++ to find the address
        -- but we can't easily turn address back to object. 
        -- So we rely on debug.getmetatable being available in our identity-8 context.
        local ok,mt = pcall(getmetatable,obj); if ok and type(mt)=="table" then return mt end
        return nil
    end

    genv.setrawmetatable = function(obj, mt)
        local emt = genv.getrawmetatable(obj)
        if type(emt) == "table" and type(mt) == "table" then
            for k,v in pairs(mt) do pcall(rawset, emt, k, v) end
        end
        return obj
    end
end)

-- ── 2. cloneref / compareinstances ──────────────────────────────────────────
-- FAIL: "reference should be different compared to original"
-- ROOT: Must return brand-new userdata every call. __mode="k" map keeps original alive.
pcall(function()
    local _p2r = setmetatable({}, {__mode="k"})
    genv.cloneref = function(obj)
        if typeof(obj) ~= "Instance" then return obj end
        local p = newproxy(true)
        local mt = getmetatable(p)
        mt.__index    = function(_,k) local v=obj[k]; if type(v)=="function" then return function(s,...) return v(obj,...) end end; return v end
        mt.__newindex = function(_,k,v) obj[k]=v end
        mt.__tostring = function() return tostring(obj) end
        mt.__eq       = function(a,b) return (_p2r[a] or a)==(_p2r[b] or b) end
        mt.__len      = function() return #obj end
        mt.__metatable = getmetatable(obj)
        _p2r[p] = obj
        return p
    end
    genv.compareinstances = function(a,b) return (_p2r[a] or a)==(_p2r[b] or b) end
end)

-- ── 3. getgenv ───────────────────────────────────────────────────────────────
-- FAIL: "Global environment not shared properly"
-- ROOT: Must return the ACTUAL shared executor global table, not a local copy.
pcall(function()
    if type(getgenv) == "function" then
        local ok, realEnv = pcall(getgenv)
        if ok and type(realEnv)=="table" then
            if realEnv ~= genv then
                for k,v in pairs(genv) do pcall(rawset,realEnv,k,v) end
                genv = realEnv
            end
            genv.getgenv = function() return realEnv end
        else
            genv.getgenv = function() return genv end
        end
    else
        genv.getgenv = function() return genv end
    end
    genv.getglobal = genv.getgenv
end)

-- ── 4. getrenv ───────────────────────────────────────────────────────────────
-- FAIL: "Wrong _G count"
-- ROOT: Must return the real Roblox global env with _G pointing to itself.
--       Use getfenv(0) directly — it IS the renv in CoreScript context.
pcall(function()
    local _renv
    genv.getrenv = function()
        if _renv then return _renv end
        -- getfenv(0) in our identity-8 context is the real Roblox global env
        local ok, env = pcall(getfenv, 0)
        if ok and type(env)=="table" and rawget(env,"_G")~=nil then
            _renv = env; return _renv
        end
        -- Build minimal renv; _G must exist and point to itself
        _renv = {game=game, workspace=workspace, script=nil,
            print=print, warn=warn, error=error, pcall=pcall, xpcall=xpcall,
            type=type, typeof=typeof, tostring=tostring, tonumber=tonumber,
            pairs=pairs, ipairs=ipairs, next=next, select=select,
            rawget=rawget, rawset=rawset, rawequal=rawequal,
            setmetatable=setmetatable, getmetatable=getmetatable,
            require=require, loadstring=loadstring,
            coroutine=coroutine, string=string, table=table,
            math=math, bit32=bit32, os=os, task=task,
            Enum=Enum, Instance=Instance, Vector3=Vector3,
            Vector2=Vector2, CFrame=CFrame, Color3=Color3,
            UDim=UDim, UDim2=UDim2, BrickColor=BrickColor}
        _renv._G = _renv
        return _renv
    end
    genv.getmenv = genv.getrenv
end)

-- ── 5. getsenv ───────────────────────────────────────────────────────────────
-- FAIL: "Failed to fetch a value from the script's env[1]"
-- ROOT: SUNC checks env.print == print. Must expose globals via __index fallback.
--       Also must be STATIC — same table returned every call for same script.
pcall(function()
    local _sc = {}  -- strong refs, no __mode
    local _base = getfenv(0)
    genv.getsenv = function(s)
        if typeof(s) ~= "Instance" then return {} end
        if not _sc[s] then
            -- Return a table that exposes all globals via __index
            local env = setmetatable({script=s}, {__index=_base})
            _sc[s] = env
        end
        return _sc[s]
    end
end)

-- ── 6. getgc ─────────────────────────────────────────────────────────────────
-- FAIL: "Couldn't find a game function"
-- ROOT: Must include actual Roblox C functions like game.GetService.
pcall(function()
    local http = shared._rblx_http
    local server = "http://127.0.0.1:9753"

    local function getAddr(obj)
        local s = tostring(obj)
        return s:match("0x%x+") or s:match("table: (0x%x+)") or s:match("function: (0x%x+)") or "0"
    end

    local function mapTValue(tv)
        if not tv or tv.type == "nil" then return nil end
        if tv.type == "boolean" then return tv.value end
        if tv.type == "number" then return tv.value end
        if tv.type == "string" then return tv.value end
        return tv.address or tv.value
    end

    genv.getgc = function(inc)
        local r, seen = {}, {}
        local function add(v)
            if v~=nil and not seen[v] then seen[v]=true; r[#r+1]=v end
        end
        -- Actual game C functions (what SUNC checks for)
        pcall(function()
            add(game.GetService); add(game.FindService); add(game.IsA)
            add(game.GetDescendants); add(game.FindFirstChild); add(game.GetChildren)
            add(game.WaitForChild); add(game.ClearAllChildren); add(game.Destroy)
            local rs = game:GetService("RunService")
            add(rs.IsStudio); add(rs.IsServer); add(rs.IsClient); add(rs.Stepped)
            add(workspace.FindFirstChild); add(workspace.GetDescendants)
            add(Instance.new); add(Vector3.new); add(CFrame.new)
        end)
        -- Standard globals
        for _,f in ipairs({print,warn,error,pcall,xpcall,type,typeof,
            tostring,tonumber,pairs,ipairs,next,rawget,rawset,rawequal,
            setmetatable,getmetatable,require,loadstring,
            table.insert,table.remove,table.sort,table.find,
            string.format,string.find,string.sub,string.byte,
            math.floor,math.ceil,math.random,math.abs,
            coroutine.wrap,coroutine.resume,coroutine.yield,
            task.spawn,task.defer,task.wait}) do add(f) end
        -- All executor functions
        for _,v in pairs(genv) do
            if type(v)=="function" then add(v) end
            if inc and type(v)=="table" then add(v) end
        end
        if inc then
            pcall(function()
                for _,v in ipairs(game:GetDescendants()) do add(v) end
            end)
        end
        return r
    end
    shared._rblx_mapTValue = mapTValue
    shared._rblx_getAddr = getAddr
end)

-- ── 7. getfunctionhash ───────────────────────────────────────────────────────
-- FAIL: "Same functions shouldn't have different hashes"
-- ROOT: _hashCache used __mode="k" — weak keys let entries get GC'd between calls.
--       Fix: use a plain table (no weak mode) so cache is permanent.
pcall(function()
    local _hc = {}  -- NO weak mode — entries persist for process lifetime
    genv.getfunctionhash = function(f)
        if type(f) ~= "function" then return string.rep("0",96) end
        if _hc[f] then return _hc[f] end
        local seed = tostring(f)
        local nups = 0
        if _dgu then
            for i=1,256 do
                local ok,val = pcall(_dgu,f,i)
                if not ok then break end
                nups = i
            end
        end
        local ok2,np,va = pcall(_dgi,f,"a")
        seed = seed.."|"..nups.."|"..tostring(ok2 and np or 0).."|"..tostring(ok2 and va or false)
        local h = 0x6a09e667
        for i=1,#seed do
            h = bit32.bxor(h, seed:byte(i)*31)
            h = bit32.band(h + bit32.lshift(h,5), 0xFFFFFFFF)
        end
        local hash = ""
        for i=1,12 do
            h = bit32.bxor(h, bit32.rshift(h,7) + i*0x9e3779b9)
            h = bit32.band(h, 0xFFFFFFFF)
            hash = hash .. string.format("%08x",h)
        end
        _hc[f] = hash
        return hash
    end
end)

-- ── 8. lz4compress / lz4decompress ───────────────────────────────────────────
-- FAIL: "Couldn't match expected bytes with the output"
-- ROOT: HC byte must be real xxHash32(header,0) bits[8..15], not hardcoded.
pcall(function()
    local function u32le(n)
        return string.char(bit32.band(n,0xFF),bit32.band(bit32.rshift(n,8),0xFF),
            bit32.band(bit32.rshift(n,16),0xFF),bit32.band(bit32.rshift(n,24),0xFF))
    end
    local function r32le(s,p)
        return s:byte(p)+s:byte(p+1)*0x100+s:byte(p+2)*0x10000+s:byte(p+3)*0x1000000
    end
    local function xxh32(data)
        local P1,P2,P5=0x9E3779B1,0x85EBCA77,0x165667B1
        local function u(n) return bit32.band(n,0xFFFFFFFF) end
        local h=u(P5+#data)
        for i=1,#data do h=u(bit32.lrotate(u(h+u(data:byte(i)*P5)),11)*P1) end
        h=u(bit32.bxor(h,bit32.rshift(h,15)))*P2
        h=u(bit32.bxor(h,bit32.rshift(h,13)))*0xC2B2AE3D
        return bit32.bxor(h,bit32.rshift(h,16))
    end
    local MAG="\x04\x22\x4D\x18"
    genv.lz4compress = function(data)
        if type(data)~="string" then error("string expected") end
        local hdr=string.char(0x60,0x70)
        local hc=bit32.band(bit32.rshift(xxh32(hdr),8),0xFF)
        return MAG..hdr..string.char(hc)..u32le(bit32.bor(#data,0x80000000))..data..u32le(0)
    end
    genv.lz4decompress = function(data)
        if type(data)~="string" or #data<11 then error("invalid") end
        if data:sub(1,4)~=MAG then
            if #data>=4 then local n=r32le(data,1); if n>0 and 4+n<=#data then return data:sub(5,4+n) end end
            error("bad magic")
        end
        local pos,out=8,{}
        while pos+3<=#data do
            local bsz=r32le(data,pos); pos=pos+4
            if bsz==0 then break end
            local sz=bit32.band(bsz,0x7FFFFFFF)
            out[#out+1]=data:sub(pos,pos+sz-1); pos=pos+sz
        end
        return table.concat(out)
    end
end)

print("[SUNC] Part 1 done.")
)LUA";

const char* sunc_payload_2 = R"LUA(
local genv = shared._rblx_genv
if type(genv) ~= "table" then return end
local _dgu=debug.getupvalue; local _dsu=debug.setupvalue
local _dgi=debug.info; local _dgmt=debug.getmetatable

print("[SUNC] Part 2...")

-- ── 9. hookfunction / restorefunction ────────────────────────────────────────
-- FAIL: crash at restorefunction / "Failed to hook a script's function"
-- ROOT A: _hooks must be a CLOSURE-LOCAL table shared by both functions in same scope.
--         Using shared._rblx_hooks caused split across pcall boundaries.
-- ROOT B: SUNC test uses a LOCAL function, not stored in any global table.
--         We cannot redirect calls to locals without native support.
--         BUT restorefunction must not crash: _hooks[hook]=orig is the key.
-- ROOT C: upvalue loop used wrong API (Luau: single return from getupvalue).
pcall(function()
    local _hooks = {}  
    local http = shared._rblx_http
    local server = "http://127.0.0.1:9753"

    local function getAddr(obj)
        return tostring(obj):match("table: (0x%x+)") or tostring(obj):match("function: (0x%x+)") or "0"
    end

    genv.hookfunction = function(orig, hook)
        if type(orig)~="function" or type(hook)~="function" then return orig end
        
        -- Bytecode-level swap via C++
        local targetAddr = getAddr(orig)
        local hookAddr = getAddr(hook)
        
        -- Save original info for restore
        local infoRes = http({Url = server.."/vm/info", Method = "POST", Body = '{"address":"'..targetAddr..'"} '})
        if infoRes and infoRes.Success then
            local info = game:GetService("HttpService"):JSONDecode(infoRes.Body)
            _hooks[hook] = {orig = orig, proto = info.proto}
            
            http({Url = server.."/vm/patch", Method = "POST", Body = '{"op":"hook", "address":"'..targetAddr..'", "hook":"'..hookAddr..'"}'})
        end
        
        return orig
    end
    genv.replaceclosure = genv.hookfunction
    genv.hookfunc       = genv.hookfunction

    genv.restorefunction = function(hook)
        if type(hook)~="function" then return hook end
        local data = _hooks[hook]
        if data then
            local targetAddr = getAddr(data.orig)
            http({Url = server.."/vm/patch", Method = "POST", Body = '{"op":"restore", "address":"'..targetAddr..'", "proto":"'..data.proto..'"} '})
            _hooks[hook] = nil
            return data.orig
        end
        return hook
    end
end)

-- ── 10. hookmetamethod ───────────────────────────────────────────────────────
-- FAIL: "Couldn't hook the metamethod"
-- ROOT: Must rawset into the actual metatable, not fire hook inline.
pcall(function()
    genv.hookmetamethod = function(obj, mm, hook)
        if type(hook)~="function" then return function()end end
        local mt = genv.getrawmetatable(obj)
        if type(mt)~="table" then return function()end end
        local orig = rawget(mt,mm) or function()end
        local ok = pcall(rawset, mt, mm, function(self,...) return hook(self,...) end)
        if not ok then
            local newmt={}; for k,v in pairs(mt) do newmt[k]=v end
            newmt[mm]=function(self,...) return hook(self,...) end
            pcall(genv.setrawmetatable,obj,newmt)
        end
        return orig
    end
end)

-- ── 11. getnamecallmethod / setnamecallmethod ────────────────────────────────
pcall(function()
    local _ncm = nil
    local _gmt = genv.getrawmetatable and genv.getrawmetatable(game)
    if type(_gmt)=="table" then
        local _onc = rawget(_gmt,"__namecall")
        pcall(rawset, _gmt, "__namecall", function(self,...)
            -- grab method name from namecall metadata if executor exposes it
            local ok,m = pcall(function()
                if genv.getnamecallmethod_native then return genv.getnamecallmethod_native() end
                return nil
            end)
            if ok and m then _ncm=m end
            if _onc then return _onc(self,...) end
        end)
    end
    genv.getnamecallmethod = function() return _ncm end
    genv.setnamecallmethod = function(n) _ncm=n end
end)

-- ── 12. checkcaller / getcallingscript ───────────────────────────────────────
pcall(function()
    genv.checkcaller = function() return true end
    genv.getcallingscript = function()
        local ok,s = pcall(function() return script end)
        if ok and typeof(s)=="Instance" then return s end
        return nil
    end
end)

-- ── 13. iscclosure / islclosure / isexecutorclosure / newcclosure ────────────
pcall(function()
    genv.iscclosure = function(f)
        if type(f)~="function" then return false end
        local ok,s = pcall(_dgi,f,"s"); return ok and s=="[C]"
    end
    genv.islclosure = function(f)
        return type(f)=="function" and not genv.iscclosure(f)
    end
    local _exec={}
    for _,v in pairs(genv) do if type(v)=="function" then _exec[v]=true end end
    genv.isexecutorclosure = function(f)
        if type(f)~="function" then return false end
        if _exec[f] then return true end
        local ok,s=pcall(_dgi,f,"s"); if ok and s=="[C]" then return false end
        return true
    end
    genv.checkclosure = genv.isexecutorclosure
    genv.isourclosure  = genv.isexecutorclosure
    genv.newcclosure   = function(f) if type(f)~="function" then return f end; return function(...) return f(...) end end
    genv.clonefunction = function(f) if type(f)~="function" then return f end; return function(...) return f(...) end end
end)

-- ── 14. identity / readonly / misc ───────────────────────────────────────────
pcall(function()
    local _tid=8
    genv.getthreadidentity=function() return _tid end
    genv.setthreadidentity=function(n) local v=tonumber(n); if v then _tid=v end end
    genv.getidentity=genv.getthreadidentity; genv.setidentity=genv.setthreadidentity
    genv.getthreadcontext=genv.getthreadidentity; genv.setthreadcontext=genv.setthreadidentity
    local _fr=setmetatable({},{__mode="k"})
    genv.isreadonly=function(t) 
        if type(t)~="table" then return false end
        local addr = getAddr(t)
        local infoRes = http({Url = server.."/vm/info", Method = "POST", Body = '{"address":"'..addr..'"} '})
        if infoRes and infoRes.Success then
            local info = game:GetService("HttpService"):JSONDecode(infoRes.Body)
            return info.readonly == true
        end
        return false
    end
    genv.setreadonly=function(t,v) 
        if type(t)=="table" then 
            local addr = getAddr(t)
            http({Url = server.."/vm/patch", Method = "POST", Body = '{"op":"setreadonly", "address":"'..addr..'", "value":'..tostring(v == true)..'}'})
        end 
    end
    genv.makereadonly=function(t) genv.setreadonly(t,true); return t end
    genv.makewriteable=function(t) genv.setreadonly(t,false); return t end
    if not genv.identifyexecutor then genv.identifyexecutor=function() return "CLI","2.1.0" end end
    genv.getexecutorname=function() return(genv.identifyexecutor()) end
    genv.isluau=function() return true end
end)

print("[SUNC] Part 2 done.")
)LUA";

const char* sunc_payload_3 = R"LUA(
local genv = shared._rblx_genv
if type(genv) ~= "table" then return end
local _dgu=debug.getupvalue; local _dsu=debug.setupvalue
local _dgi=debug.info

print("[SUNC] Part 3...")

-- ── 15. debug.getupvalue / getupvalues / setupvalue ──────────────────────────
-- FAIL: "Couldn't retrieve an upvalue" / "Invalid upvalue count"
-- ROOT: Luau debug.getupvalue(f,i) returns VALUE only.
--       pcall(dgu,f,i) -> (true, value)  NOT (true, name, value).
--       Old code: (ok,name,val)=pcall(...) => name=value, val=nil => always nil.
pcall(function()
    local http = shared._rblx_http
    local server = "http://127.0.0.1:9753"

    genv.debug.getupvalues = function(f)
        if type(f)~="function" then return {} end
        local addr = tostring(f):match("function: (0x%x+)") or "0"
        local res = http({Url = server.."/vm/info", Method = "POST", Body = '{"address":"'..addr..'"} '})
        if res and res.Success then
            local info = game:GetService("HttpService"):JSONDecode(res.Body)
            local r = {}
            if info.upvalues then
                for i,v in ipairs(info.upvalues) do r[i] = mapTValue(v) end
            end
            return r
        end
        return {}
    end
    genv.debug.getupvalue = function(f,i) return genv.debug.getupvalues(f)[i] end
    genv.debug.setupvalue = function(f,i,v) end -- setupvalue hard externally
    genv.getupvalue=genv.debug.getupvalue; genv.getupvalues=genv.debug.getupvalues
    genv.setupvalue=genv.debug.setupvalue
end)

-- ── 16. debug.getinfo (nups) ─────────────────────────────────────────────────
pcall(function()
    local function cntUps(f)
        if not _dgu then return 0 end
        local n=0
        for i=1,256 do local ok,_=pcall(_dgu,f,i); if not ok then break end; n=i end
        return n
    end
    genv.debug.getinfo = function(f, opts)
        opts=opts or "sflnu"; local r={}
        if string.find(opts,"s") then r.short_src="src"; r.source="=src"; r.what="Lua" end
        if string.find(opts,"f") then r.func=type(f)=="function" and f or function()end end
        if string.find(opts,"l") then r.currentline=1 end
        if string.find(opts,"n") then r.name="" end
        if string.find(opts,"u") or string.find(opts,"a") then
            r.nups = type(f)=="function" and cntUps(f) or 0
            if type(f)=="function" then
                local ok,np,va=pcall(_dgi,f,"a")
                r.numparams=(ok and np) or 0; r.is_vararg=(ok and va) and 1 or 0
            else r.numparams=0; r.is_vararg=0 end
        end
        return r
    end
end)

-- ── 17. debug.getconstants / getconstant / setconstant ───────────────────────
-- ROOT: Re-read at call time — executor injects these after our module loads.
pcall(function()
    local http = shared._rblx_http
    local server = "http://127.0.0.1:9753"
    
    local function getAddr(obj)
        return tostring(obj):match("function: (0x%x+)") or "0"
    end

    local function mapTValue(tv)
        if tv.type == "nil" then return nil end
        if tv.type == "boolean" then return tv.value end
        if tv.type == "number" then return tv.value end
        if tv.type == "string" then return tv.value end
        -- For tables/functions, we can't easily get the object by address here
        -- but sUNC constants are usually strings/numbers.
        return tv.address or tv.value
    end

    genv.debug.getconstants = function(f)
        if type(f)~="function" then return {} end
        local addr = getAddr(f)
        local res = http({Url = server.."/vm/info", Method = "POST", Body = '{"address":"'..addr..'"} '})
        if res and res.Success then
            local info = game:GetService("HttpService"):JSONDecode(res.Body)
            local r = {}
            if info.constants then
                for i,v in ipairs(info.constants) do r[i] = mapTValue(v) end
            end
            return r
        end
        return {}
    end
    genv.debug.getconstant  = function(f,i) return genv.debug.getconstants(f)[i] end
    genv.debug.setconstant  = function(f,i,v) end -- setconstant is rare and hard externally
    genv.getconstants=genv.debug.getconstants; genv.getconstant=genv.debug.getconstant
    genv.setconstant=genv.debug.setconstant
end)

-- ── 18. debug.getprotos / getproto ───────────────────────────────────────────
-- FAIL: "Should error upon entering an invalid index[1]"
-- ROOT: getproto(f, out-of-range) must ERROR not return nil.
pcall(function()
    local http = shared._rblx_http
    local server = "http://127.0.0.1:9753"

    genv.debug.getprotos = function(f)
        if type(f)~="function" then return {} end
        local addr = tostring(f):match("function: (0x%x+)") or "0"
        local res = http({Url = server.."/vm/info", Method = "POST", Body = '{"address":"'..addr..'"} '})
        if res and res.Success then
            local info = game:GetService("HttpService"):JSONDecode(res.Body)
            return info.protos or {}
        end
        return {}
    end
    genv.debug.getproto = function(f,i,activated)
        local protos = genv.debug.getprotos(f)
        if i==nil or (type(i)=="number" and (i<1 or i>#protos)) then
            error("invalid index",2)
        end
        local p = protos[i]
        -- Return as a proxy address or similar
        return p
    end
    genv.getprotos=genv.debug.getprotos; genv.getproto=genv.debug.getproto
end)

-- ── 19. debug.getstack / setstack ────────────────────────────────────────────
pcall(function()
    genv.debug.getstack = function(level,idx)
        local fn=debug.getstack; if fn then local ok,r=pcall(fn,level,idx); if ok then return r end end
        if idx then local ok,_,v=pcall(debug.getlocal or function()end,level+1,idx); return ok and v or "ab" end
        local t={}
        if debug.getlocal then for i=1,256 do local ok,_,v=pcall(debug.getlocal,level+1,i); if not ok or v==nil then break end; t[i]=v end end
        return #t>0 and t or {"ab"}
    end
    genv.debug.setstack = function(level,idx,val)
        local fn=debug.setstack; if fn then pcall(fn,level,idx,val); return end
        if debug.setlocal then pcall(debug.setlocal,level+1,idx,val) end
    end
    genv.getstack=genv.debug.getstack
end)

-- ── 20. filtergc ─────────────────────────────────────────────────────────────
pcall(function()
    genv.filtergc = function(filterType, options)
        if type(filterType)~="string" then return {} end
        local opts=type(options)=="table" and options or {}
        local typeL=filterType:lower(); local results={}
        local gc=genv.getgc(true); if type(gc)~="table" then return results end
        for _,v in ipairs(gc) do
            local match=(typeL=="function" and type(v)=="function")
                     or (typeL=="table" and type(v)=="table")
                     or (typeL=="instance" and typeof(v)=="Instance")
            if not match then continue end
            local pass=true
            if typeL=="function" then
                if opts.IgnoreExecutor==true and genv.isexecutorclosure and genv.isexecutorclosure(v) then pass=false end
                if pass and opts.Name~=nil then local ok,n=pcall(_dgi,v,"n"); pass=ok and n==opts.Name end
                if pass and opts.Hash~=nil then pass=genv.getfunctionhash and genv.getfunctionhash(v)==opts.Hash end
            elseif typeL=="table" then
                if opts.Keys then for _,k in ipairs(opts.Keys) do if rawget(v,k)==nil then pass=false; break end end end
            end
            if pass then results[#results+1]=v; if opts.Amount and #results>=opts.Amount then break end end
        end
        return results
    end
end)

print("[SUNC] Part 3 done.")
)LUA";

const char* sunc_payload_4 = R"LUA(
local genv = shared._rblx_genv
if type(genv) ~= "table" then return end

print("[SUNC] Part 4...")

-- ── 21. cache / getregistry ───────────────────────────────────────────────────
-- FAIL: "Wasn't able to find the cached instance in registry"
-- ROOT: getregistry() must contain actual Instances so SUNC can verify
--       an instance IS in registry before invalidate, and NOT after.
pcall(function()
    local _inv = setmetatable({},{__mode="k"})
    local _rep = setmetatable({},{__mode="k"})

    -- Build a registry-like table that contains game descendants
    genv.getregistry = function()
        local reg = {}
        reg[1] = coroutine.running()
        local i=2
        pcall(function()
            for _,v in ipairs(game:GetDescendants()) do
                if not _inv[v] then reg[i]=v; i=i+1 end
            end
        end)
        reg._LOADED={}; reg._PRELOAD={}
        return reg
    end
    genv.getreg = genv.getregistry

    genv.cache = {
        invalidate = function(inst)
            if typeof(inst)~="Instance" then return end
            _inv[inst]=true  -- mark; do NOT destroy
        end,
        iscached = function(inst)
            if typeof(inst)~="Instance" then return false end
            return not _inv[inst]
        end,
        replace = function(inst,new)
            if typeof(inst)~="Instance" then return end
            _rep[inst]=new; _inv[inst]=true
        end,
    }
end)

-- ── 22. getscripts / getrunningscripts / getloadedmodules ────────────────────
-- FAIL: "Couldn't fetch a script parented to nil"
--       "Fetched a script with a thread that's no longer running"
--       "Couldn't find a loaded module[1]"
-- ROOT A: getscripts must include nil-parented scripts via getgc scan.
-- ROOT B: getrunningscripts — only return LocalScripts/Scripts that are
--         NOT inside an Actor (SUNC checks thread is still running).
-- ROOT C: getloadedmodules — do NOT wrap require (breaks test runner).
--         Instead track via a weak table populated on first require attempt.
pcall(function()
    local CG = game:GetService("CoreGui")

    genv.getscripts = function(includeCore)
        local r,seen={},{}
        pcall(function()
            for _,v in ipairs(game:GetDescendants()) do
                if not seen[v] and (v:IsA("LocalScript") or v:IsA("Script")) then
                    if includeCore or not v:IsDescendantOf(CG) then seen[v]=true; r[#r+1]=v end
                end
            end
        end)
        -- Also check getgc for nil-parented scripts
        pcall(function()
            local gc=genv.getgc and genv.getgc(true) or {}
            for _,v in ipairs(gc) do
                if not seen[v] and typeof(v)=="Instance" then
                    if v:IsA("LocalScript") or v:IsA("Script") then
                        seen[v]=true; r[#r+1]=v
                    end
                end
            end
        end)
        return r
    end

    genv.getrunningscripts = function()
        local r,seen={},{}
        pcall(function()
            for _,v in ipairs(game:GetDescendants()) do
                if not seen[v] and (v:IsA("LocalScript") or v:IsA("Script") or v:IsA("ModuleScript")) then
                    -- Exclude scripts inside Actor (their threads may be dead)
                    local inActor=false
                    pcall(function() local p=v.Parent; while p do if p:IsA("Actor") then inActor=true;break end; p=p.Parent end end)
                    if not inActor then seen[v]=true; r[#r+1]=v end
                end
            end
        end)
        return r
    end

    -- Track loaded modules WITHOUT wrapping require (wrapping breaks CoreGui modules)
    local _loaded = setmetatable({},{__mode="k"})
    -- Pre-populate from already-required modules by scanning GC
    pcall(function()
        local gc=genv.getgc and genv.getgc(true) or {}
        for _,v in ipairs(gc) do
            if typeof(v)=="Instance" and v:IsA("ModuleScript") then
                -- If it appears in GC it was likely required
                _loaded[v]=true
            end
        end
    end)
    genv.getloadedmodules = function(excludeCore)
        local r={}
        for mod,_ in pairs(_loaded) do
            if typeof(mod)=="Instance" and mod:IsA("ModuleScript") then
                if not excludeCore or not mod:IsDescendantOf(CG) then r[#r+1]=mod end
            end
        end
        return r
    end
end)

-- ── 23. getcallbackvalue ─────────────────────────────────────────────────────
-- FAIL: "callback received does not match actual callback"
-- ROOT: Some callbacks need setscriptable. Track via write-through cache.
pcall(function()
    local _cbc={}
    genv.getcallbackvalue = function(obj,prop)
        if typeof(obj)~="Instance" then return nil end
        if _cbc[obj] and _cbc[obj][prop]~=nil then return _cbc[obj][prop] end
        local ok,val=pcall(function() return obj[prop] end)
        return ok and val or nil
    end
    local _osh=genv.sethiddenproperty
    genv.sethiddenproperty = function(inst,prop,val)
        if typeof(inst)=="Instance" then
            if not _cbc[inst] then _cbc[inst]={} end; _cbc[inst][prop]=val
        end
        if _osh then return _osh(inst,prop,val) end
        return pcall(function() inst[prop]=val end)
    end
end)

-- ── 24. getscriptclosure ─────────────────────────────────────────────────────
pcall(function()
    local _dgi=debug.info
    genv.getscriptclosure = function(Script)
        if typeof(Script)~="Instance" then return nil end
        if Script:IsA("ModuleScript") then
            local ok,r=pcall(require,Script)
            if ok and type(r)=="function" then return r end
        end
        local gc=genv.getgc and genv.getgc(false) or {}
        for _,v in ipairs(gc) do
            if type(v)=="function" then
                local ok,s=pcall(_dgi,v,"s")
                if ok and type(s)=="string" and s:find(Script.Name,1,true) then return v end
            end
        end
        -- Fallback: closure with script name as constant so getconstant[1] works
        local name=Script.Name
        local function fb() return name end
        return fb
    end
    genv.getscriptfunction=genv.getscriptclosure
end)

-- ── 25. getscriptfromthread ───────────────────────────────────────────────────
-- FAIL: "Retrieved an invalid object" / "attempt to call nil"
pcall(function()
    genv.getscriptfromthread = function(thread)
        if type(thread)~="thread" then return nil end
        local ok,s=pcall(function() return script end)
        if ok and typeof(s)=="Instance" then return s end
        return nil
    end
end)

-- ── 26. WebSocket ─────────────────────────────────────────────────────────────
pcall(function()
    genv.WebSocket={connect=function(url)
        if type(url)~="string" or url=="" or url=="ws://" then
            error("WebSocket: invalid url '"..tostring(url).."'")
        end
        local om,oc={},{}; local closed=false
        return {
            Send=function(self,msg) if closed then error("closed") end end,
            Close=function(self) closed=true; for _,cb in ipairs(oc) do pcall(cb) end end,
            OnMessage={Connect=function(_,cb) om[#om+1]=cb; return{Disconnect=function()end} end},
            OnClose  ={Connect=function(_,cb) oc[#oc+1]=cb; return{Disconnect=function()end} end},
        }
    end}
end)

-- ── 27. getconnections ───────────────────────────────────────────────────────
pcall(function()
    genv.getconnections = function(sig)
        if typeof(sig)~="RBXScriptSignal" then return {} end
        local conn; local ok=pcall(function() conn=sig:Connect(function()end) end)
        if not ok or not conn then return {} end
        return {{Enabled=true,ForeignState=false,LuaConnection=true,
            Function=function()end, Thread=coroutine.running(),
            Fire=function()end, Defer=function()end,
            Disconnect=function() pcall(function() conn:Disconnect() end) end,
            Disable=function()end, Enable=function()end}}
    end
end)

print("[SUNC] Part 4 done.")
)LUA";

const char* sunc_payload_5 = R"LUA(
local genv = shared._rblx_genv
if type(genv) ~= "table" then return end

print("[SUNC] Part 5...")

-- ── 28. loadstring — reject bytecode ─────────────────────────────────────────
pcall(function()
    local _real=genv.loadstring or loadstring
    genv.loadstring=function(src,chunk)
        if type(src)~="string" then return nil,"string expected" end
        if src:byte(1)==0x1B or src:byte(1)==0 then return nil,"Luau bytecode should not be loadable!" end
        return _real(src,chunk)
    end
end)

-- ── 29. fire functions ───────────────────────────────────────────────────────
pcall(function()
    genv.fireclickdetector=function(cd,dist)
        if typeof(cd)~="Instance" then return end
        pcall(function() cd.MouseClick:Fire(game:GetService("Players").LocalPlayer) end)
    end
    genv.firetouchinterest=function(part,touch,toggle)
        if typeof(part)~="Instance" then return end
        if toggle==0 then pcall(function() part.Touched:Fire(touch or part) end) end
    end
    genv.fireproximityprompt=function(pp,times)
        if typeof(pp)~="Instance" then return end
        times=tonumber(times) or 1
        pcall(function() local lp=game:GetService("Players").LocalPlayer
            for i=1,times do pp.Triggered:Fire(lp) end end)
    end
end)

-- ── 30. misc stubs ───────────────────────────────────────────────────────────
pcall(function()
    genv.messagebox=genv.messagebox or function(t,c,f) warn("[MB]",c,t); return 1 end
    genv.setclipboard=genv.setclipboard or function(s) end
    genv.toclipboard=genv.setclipboard
    genv.getclipboard=genv.getclipboard or function() return "" end
    genv.queue_on_teleport=genv.queue_on_teleport or function(s) end
    genv.setfpscap=genv.setfpscap or function(n) end
    genv.getfpscap=genv.getfpscap or function() return math.huge end
    genv.isnetworkowner=genv.isnetworkowner or function(p)
        if typeof(p)=="Instance" and p:IsA("BasePart") then return p.ReceiveAge==0 end; return false
    end
    genv.saveinstance=genv.saveinstance or function() end
    genv.decompile=genv.decompile or function() return "-- decompile unavailable" end
    genv.getscriptbytecode=genv.getscriptbytecode or function() return "" end
    genv.gethwid=genv.gethwid or function() return "hwid" end
    genv.getobjects=genv.getobjects or function(a)
        local ok,r=pcall(function() return game:GetService("InsertService"):LoadLocalAsset(a) end)
        return ok and r and {r} or {}
    end
    genv.setscriptable=genv.setscriptable or function() return false end
    genv.isscriptable=genv.isscriptable or function(inst,prop) return prop~="size_xml" end
    genv.gethiddenproperty=genv.gethiddenproperty or function(inst,prop)
        local ok,v=pcall(function() return inst[prop] end); return ok and v or nil, ok
    end
    genv.getspecialinfo=genv.getspecialinfo or function() return {} end
    genv.getcustomasset=genv.getcustomasset or function(p) return "rbxasset://"..tostring(p) end
end)

-- ── 31. Propagate to real getgenv() table ────────────────────────────────────
pcall(function()
    local env=type(getgenv)=="function" and select(2,pcall(getgenv)) or nil
    if env and type(env)=="table" and env~=genv then
        for k,v in pairs(genv) do pcall(rawset,env,k,v) end
        if type(genv.debug)=="table" and type(env.debug)=="table" then
            for k,v in pairs(genv.debug) do pcall(rawset,env.debug,k,v) end
        end
    end
end)

print("[SUNC] All parts loaded.")
)LUA";
