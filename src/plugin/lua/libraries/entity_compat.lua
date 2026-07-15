-- Neverlose-style entity compatibility for CS2 Source 2.
local ffi = ffi or require('ffi')
pcall(ffi.cdef, [[
void* __stdcall GetModuleHandleA(const char* name);
typedef struct { float x, y, z; } cs2lua_vec3_t;
]])

entity = entity or {}
local methods, mt = {}, {}
local function cfg() return entity_offsets end
local function add(p,n) return ffi.cast('uint8_t*',p)+n end
local function readable(p,size)
  return p~=nil and utils.is_readable(tonumber(ffi.cast('uintptr_t',p)),size or 8)
end
local function read(p,n,t)
  local address=p and add(p,n) or nil
  if not readable(address,ffi.sizeof(t)) then return nil end
  return ffi.cast(t..'*',address)[0]
end
local function ptr(p,n)
  local address=p and add(p,n or 0) or nil
  if not readable(address,ffi.sizeof('void*')) then return nil end
  local v=ffi.cast('void**',address)[0]
  return v~=nil and readable(v,1) and v or nil
end
local function base() local p=ffi.C.GetModuleHandleA('client.dll'); return p~=nil and ffi.cast('uint8_t*',p) or nil end
local function rip(signature,instruction_offset,displacement_offset,instruction_size)
  local match=utils.opcode_scan('client.dll',signature)
  if match==nil then return nil end
  local instruction=add(match,instruction_offset or 0)
  local displacement=read(instruction,displacement_offset,'int32_t')
  return displacement and add(instruction,instruction_size+displacement) or nil
end
local globals,globals_resolved={},false
local function resolve_globals()
  if globals_resolved then return end
  globals_resolved=true
  globals.entity_list=rip('48 89 0D ? ? ? ? E9 ? ? ? ? CC',0,3,7)
  globals.local_controller=rip('48 8B 05 ? ? ? ? 41 89 BE',0,3,7)
  globals.game_rules=rip('F6 C1 01 0F 85 ? ? ? ? 4C 8B 05 ? ? ? ? 4D 85',9,3,7)
end
local function global_ptr(name,offset)
  resolve_globals()
  local address=globals[name]
  if address then local value=ptr(address,0); if value then return value end end
  local b=base(); return b and ptr(b,offset) or nil
end
local layout={stride=0x78,pointer_offset=0,resolved=false,validated=false}
local function resolve_layout()
  -- Do not permanently cache a failed lobby/loading-screen resolution.  The
  -- entity list and local pawn do not exist yet there; retry after map entry.
  if layout.validated then return end
  layout.resolved=true
  local list=global_ptr('entity_list',cfg().dwEntityList)
  local controller=global_ptr('local_controller',cfg().dwLocalPlayerController)
  if not list or not controller then return end
  local handle=read(controller,cfg().controller.m_hPlayerPawn,'uint32_t')
  if not handle then return end
  local index=tonumber(handle)%0x8000
  local chunk=ptr(list,0x10+8*math.floor(index/512)); if not chunk then return end
  local slot=index%512
  -- Entity-system entry stride has changed between CS2 builds. Locate it by
  -- requiring the local pawn candidate to agree with cached controller data.
  local cached_health=read(controller,cfg().controller.m_iPawnHealth,'int')
  for stride=0x60,0x98,8 do
    for pointer_offset=0,0x20,8 do
      local candidate=ptr(chunk,stride*slot+pointer_offset)
      if candidate then
        local health=read(candidate,cfg().base.m_iHealth,'int')
        local team=read(candidate,cfg().base.m_iTeamNum,'uint8_t')
        local scene=ptr(candidate,cfg().base.m_pGameSceneNode)
        if health and health>0 and health<=100 and health==cached_health and
            team and team>=1 and team<=3 and scene then
          layout.stride,layout.pointer_offset,layout.validated=stride,pointer_offset,true
          return
        end
      end
    end
  end
end
local function raw_entity(index)
  index=tonumber(index); local b=base()
  if not b or not index or index<0 or index>0x7fff then return nil end
  resolve_layout()
  local list=global_ptr('entity_list',cfg().dwEntityList); if not list then return nil end
  local chunk=ptr(list,0x10+8*math.floor(index/512)); if not chunk then return nil end
  return ptr(chunk,layout.stride*(index%512)+layout.pointer_offset)
end
local function from_handle(handle)
  if handle==nil then return nil end
  local index=tonumber(handle)%0x8000
  if index==0 or index==0x7fff then return nil end
  return raw_entity(index),index
end
local function wrap(p,index,kind,controller,pawn_index)
  if not p then return nil end
  return setmetatable({_ptr=p,_index=index or 0,_kind=kind or 'entity',
    _controller=controller,_pawn_index=pawn_index},mt)
end
local function vec(p,n) local v=read(p,n,'cs2lua_vec3_t'); return v and vector(v.x,v.y,v.z) or vector() end
local function node(self) return ptr(self._ptr,cfg().base.m_pGameSceneNode) end
local function pawn(controller,index)
  local p,i=from_handle(read(controller,cfg().controller.m_hPlayerPawn,'uint32_t'))
  -- Neverlose exposes the logical player/controller index through get_index(),
  -- not Source 2's unrelated pawn entity index. Keep the pawn pointer for all
  -- player methods and retain its real index internally.
  return wrap(p,index,'player',wrap(controller,index,'controller'),i)
end

function entity.get(index,by_userid)
  index=tonumber(index); if not index then return nil end
  if by_userid then
    -- Current CS2 legacy events expose a zero-based player userid/slot
    -- (0..63), while controller entities occupy indices 1..64. Resolve
    -- userid + 1 -> controller -> pawn, but preserve the zero-based userid as
    -- the public Neverlose-compatible get_index() value.
    index=index%0x8000
    if index==0x7fff or index>63 then return nil end
    local controller=raw_entity(index+1)
    return controller and pawn(controller,index) or nil
  end
  return wrap(raw_entity(index),index)
end
function entity.get_local_player()
  local b=base()
  if not b then return nil end

  -- Resolve all values on every call. Only addresses/entry layout are cached;
  -- controller, handle and pawn pointers are deliberately never cached because
  -- CS2 replaces/clears them on connect, disconnect and map transitions.
  resolve_layout()
  local controller=global_ptr('local_controller',cfg().dwLocalPlayerController)
  local direct_pawn=ptr(b,cfg().dwLocalPlayerPawn)
  local pawn_pointer,pawn_index
  if controller then
    pawn_pointer,pawn_index=from_handle(read(controller,cfg().controller.m_hPlayerPawn,'uint32_t'))
  end
  if not pawn_pointer then pawn_pointer=direct_pawn end
  if not pawn_pointer then return nil end

  -- A disconnected lobby may leave readable old allocations behind. Require
  -- the current pawn to still have the basic live entity structure.
  local team=read(pawn_pointer,cfg().base.m_iTeamNum,'uint8_t')
  local scene=ptr(pawn_pointer,cfg().base.m_pGameSceneNode)
  local health=read(pawn_pointer,cfg().base.m_iHealth,'int')
  if not team or team<1 or team>3 or not scene or health==nil or health<0 or health>100 then
    return nil
  end

  local controller_index=0
  if controller then
    for i=1,64 do
      if raw_entity(i)==controller then controller_index=i; break end
    end
    -- The global still points at the previous map's controller after a
    -- disconnect for a short time. It is valid only while present in the
    -- current entity list.
    if controller_index==0 and not direct_pawn then return nil end
  end

  if not pawn_index then
    for i=1,2047 do
      if raw_entity(i)==pawn_pointer then pawn_index=i; break end
    end
  end
  local public_index=controller_index~=0 and (controller_index-1) or (pawn_index or 0)
  return wrap(pawn_pointer,public_index,'player',
    controller and wrap(controller,public_index,'controller') or nil,pawn_index)
end
function entity.debug_status()
  local b=base()
  local status={module_loaded=b~=nil,client_base=b and tostring(ffi.cast('void*',b)) or 'nil',
    build_number=offsets and offsets.info and offsets.info.build_number or nil}
  if b then
    status.entity_list=tostring(global_ptr('entity_list',cfg().dwEntityList))
    status.local_controller=tostring(global_ptr('local_controller',cfg().dwLocalPlayerController))
    status.local_pawn=tostring(ptr(b,cfg().dwLocalPlayerPawn))
    status.game_rules=tostring(global_ptr('game_rules',cfg().dwGameRules))
    status.scan_entity_list=tostring(globals.entity_list)
    status.scan_local_controller=tostring(globals.local_controller)
    status.scan_game_rules=tostring(globals.game_rules)
    resolve_layout()
    status.entity_stride=string.format('0x%X',layout.stride)
    status.entity_pointer_offset=string.format('0x%X',layout.pointer_offset)
    status.entity_layout_validated=layout.validated
    local controller=global_ptr('local_controller',cfg().dwLocalPlayerController)
    if controller then
      local handle=read(controller,cfg().controller.m_hPlayerPawn,'uint32_t')
      local pawn_pointer,pawn_index=from_handle(handle)
      status.pawn_handle=handle and string.format('0x%08X',tonumber(handle)) or 'nil'
      status.pawn_index=pawn_index
      status.resolved_pawn=tostring(pawn_pointer)
      status.controller_health=read(controller,cfg().controller.m_iPawnHealth,'int')
      status.controller_armor=read(controller,cfg().controller.m_iPawnArmor,'int')
      status.pawn_health=pawn_pointer and read(pawn_pointer,cfg().base.m_iHealth,'int') or nil
      status.pawn_team=pawn_pointer and read(pawn_pointer,cfg().base.m_iTeamNum,'uint8_t') or nil
      status.scene_node=pawn_pointer and tostring(ptr(pawn_pointer,cfg().base.m_pGameSceneNode)) or 'nil'
      if pawn_pointer then
        local services=ptr(pawn_pointer,cfg().pawn.m_pWeaponServices)
        local weapon_handle=services and read(services,cfg().weapon_services.m_hActiveWeapon,'uint32_t') or nil
        status.weapon_services=tostring(services)
        status.weapon_handle=weapon_handle and string.format('0x%08X',tonumber(weapon_handle)) or 'nil'
      end
    end
  end
  return status
end
function entity.get_players(enemies_only,include_dormant,callback)
  local result={}
  for i=1,64 do
    local c=raw_entity(i); local e=c and pawn(c,i-1)
    if e and (include_dormant or not e:is_dormant()) and (not enemies_only or e:is_enemy()) then
      if callback then callback(e) else result[#result+1]=e end
    end
  end
  return callback and nil or result
end
function entity.get_entities(class,include_dormant,callback)
  local result={}
  for i=0,2047 do
    local e=wrap(raw_entity(i),i)
    if e and (include_dormant or not e:is_dormant()) and
      (class==nil or class==e:get_classname() or class==e:get_classid()) then
      if callback then callback(e) else result[#result+1]=e end
    end
  end
  return callback and nil or result
end
function entity.get_game_rules() return wrap(global_ptr('game_rules',cfg().dwGameRules),0,'game_rules') end
function entity.get_player_resource() return nil end
function entity.get_threat() return nil end

function methods:is_player() return self._kind=='player' or self._kind=='controller' end
function methods:is_weapon() return self._kind=='weapon' end
function methods:is_dormant() local n=node(self); return n and read(n,cfg().scene.m_bDormant,'uint8_t')~=0 or false end
function methods:is_alive() return (read(self._ptr,cfg().base.m_iHealth,'int') or 0)>0 and (read(self._ptr,cfg().base.m_lifeState,'uint8_t') or 1)==0 end
function methods:is_enemy()
  local me=entity.get_local_player(); if not me then return false end
  return read(self._ptr,cfg().base.m_iTeamNum,'uint8_t')~=read(me._ptr,cfg().base.m_iTeamNum,'uint8_t')
end
function methods:is_bot() local i=self:get_player_info(); return i and i.is_fake_player or false end
function methods:is_visible() return not self:is_dormant() end
function methods:is_occluded() return not self:is_visible() end
function methods:get_index() return self._index end
function methods:get_name()
  local source=self._controller or self
  if source._kind~='controller' then return self:get_classname() end
  return ffi.string(ffi.cast('char*',add(source._ptr,cfg().base_controller.m_iszPlayerName)),128):match('^[^%z]*') or ''
end
function methods:get_origin() local n=node(self); return n and vec(n,cfg().scene.m_vecAbsOrigin) or vec(self._ptr,cfg().pawn.m_vOldOrigin) end
function methods:get_angles() return vec(self._ptr,cfg().cs_pawn.m_angEyeAngles) end
function methods:get_eye_position() return self:get_origin()+vec(self._ptr,cfg().model.m_vecViewOffset) end
function methods:get_simulation_time() return {current=0,old=0} end
function methods:get_classname() return self._kind end
function methods:get_classid() return 0 end
function methods:get_materials() return {} end
function methods:get_model_name() return '' end
function methods:get_network_state() return self:is_dormant() and 4 or 0 end
function methods:get_bbox() return {pos1=vector(),pos2=vector(),alpha=self:is_dormant() and 0 or 1} end
function methods:get_player_info()
  local source=self._controller or self; if source._kind~='controller' then return nil end
  local steam=tostring(read(source._ptr,cfg().base_controller.m_steamID,'uint64_t') or 0):gsub('ULL$','')
  return {is_hltv=false,is_fake_player=steam=='0',steamid=steam,steamid64=steam,userid=source._index,files_downloaded=0}
end
function methods:get_player_weapon(all)
  local services=ptr(self._ptr,cfg().pawn.m_pWeaponServices)
  local p,i
  if services then p,i=from_handle(read(services,cfg().weapon_services.m_hActiveWeapon,'uint32_t')) end
  local weapon=wrap(p,i,'weapon'); return all and (weapon and {weapon} or {}) or weapon
end
function methods:get_anim_state() return {} end
function methods:get_anim_overlay(index) return index==nil and {} or nil end
function methods:get_bone_position() return self:get_origin() end
function methods:get_hitbox_position() return self:get_origin() end
function methods:get_xuid() local i=self:get_player_info(); return i and i.steamid64 or '0' end
function methods:get_resource() return entity.get_player_resource() end
function methods:get_spectators() return {} end
function methods:set_icon() return false end
function methods:get_weapon_index() return self:get_index() end
function methods:get_weapon_icon() return nil end
function methods:get_weapon_info() return nil end

local props={
  m_iHealth={'base','m_iHealth','int'},m_iMaxHealth={'base','m_iMaxHealth','int'},
  m_iTeamNum={'base','m_iTeamNum','uint8_t'},m_lifeState={'base','m_lifeState','uint8_t'},
  m_vecVelocity={'base','m_vecVelocity','cs2lua_vec3_t'},m_vecAbsVelocity={'base','m_vecAbsVelocity','cs2lua_vec3_t'}
}
function mt.__index(self,key)
  if key==0 then return self._ptr end
  if methods[key] then return methods[key] end
  local s=props[key]; if not s then return rawget(self,key) end
  local n=cfg()[s[1]][s[2]]; return s[3]=='cs2lua_vec3_t' and vec(self._ptr,n) or read(self._ptr,n,s[3])
end
function mt.__newindex(self,key,value)
  local s=props[key]; if not s then rawset(self,key,value); return end
  local n=cfg()[s[1]][s[2]]
  if s[3]=='cs2lua_vec3_t' then local v=ffi.cast('cs2lua_vec3_t*',add(self._ptr,n))[0]; v.x,v.y,v.z=value.x,value.y,value.z
  else ffi.cast(s[3]..'*',add(self._ptr,n))[0]=value end
end
function mt.__eq(a,b) return a and b and a._ptr==b._ptr end
function mt.__tostring(self) return ('entity[%d](%s)'):format(self._index,self._kind) end
