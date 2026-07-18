-- Neverlose-style compatibility layer for CS2LuaPlugin.
-- Native-backed namespaces (events/entity/render) may extend these tables.
CS2LUA_API_VERSION = '1.0.2'

-- LuaJIT uses Lua 5.1 semantics, so provide the Lua 5.3-style UTF-8 helpers
-- used by the compatibility layer and bundled example scripts.
table.unpack = table.unpack or unpack
utf8 = utf8 or {}
function utf8.char(code)
  assert(type(code)=='number' and code>=0 and code<=0x10ffff, 'invalid UTF-8 codepoint')
  if code<0x80 then return string.char(code) end
  if code<0x800 then return string.char(0xc0+math.floor(code/0x40),0x80+code%0x40) end
  if code<0x10000 then return string.char(0xe0+math.floor(code/0x1000),0x80+math.floor(code/0x40)%0x40,0x80+code%0x40) end
  return string.char(0xf0+math.floor(code/0x40000),0x80+math.floor(code/0x1000)%0x40,0x80+math.floor(code/0x40)%0x40,0x80+code%0x40)
end

-- Neverlose-style script UI. Native ImGui rendering is owned by the plugin;
-- these Lua objects preserve the documented MenuGroup/MenuItem interface.
ui = ui or {}
local ui_item_mt,ui_group_mt={},{}
local ui_registry,ui_locales,ui_hotkeys={},{},{}
local function wrap_ui_item(id,name,kind,items)
  local value={_id=id,_name=name,_type=kind,_items=items or {},_tooltip=nil,_override=nil}
  ui_registry[name]=value
  if kind=='hotkey' then ui_hotkeys[#ui_hotkeys+1]=value end
  return setmetatable(value,{__index=ui_item_mt})
end
local function option_list(...)
  local args={...}; if #args==1 and type(args[1])=='table' then return args[1] end return args
end
local function make_item(group,kind,name,initial,min,max,options)
  local id=ui_native._create_item(group._name,kind,name,initial,min,max,options)
  local item=wrap_ui_item(id,name,kind,options); group._items[#group._items+1]=item; return item
end
function ui.create(a,b,c)
  local name=b and (tostring(a)..' / '..tostring(b)) or tostring(a)
  return setmetatable({_name=name,_column=c,_items={}}, {__index=ui_group_mt})
end
function ui_group_mt:switch(name,initial) return make_item(self,'switch',name,initial or false) end
function ui_group_mt:slider(name,min,max,initial,scale,tooltip)
  local item=make_item(self,'slider',name,initial or min,min,max,scale or 1); if tooltip then item:tooltip(tooltip) end; return item
end
function ui_group_mt:combo(name,...)
  local values=option_list(...); return make_item(self,'combo',name,1,nil,nil,values)
end
function ui_group_mt:selectable(name,...)
  local values=option_list(...); return make_item(self,'selectable',name,1,nil,nil,values)
end
function ui_group_mt:list(name,...) local values=option_list(...); return make_item(self,'list',name,1,nil,nil,values) end
function ui_group_mt:listable(name,...) local values=option_list(...); return make_item(self,'listable',name,1,nil,nil,values) end
function ui_group_mt:button(name,callback,alt_style)
  local item=make_item(self,'button',name); if callback then item:set_callback(callback) end; return item
end
function ui_group_mt:hotkey(name,key) return make_item(self,'hotkey',name,key or 0) end
function ui_group_mt:input(name,text) return make_item(self,'input',name,text or '') end
function ui_group_mt:label(text) return make_item(self,'label',text) end
function ui_group_mt:color_picker(name,initial)
  -- Color storage compatibility; native picker rendering is scheduled next.
  local item=make_item(self,'color_picker',name); item._color=initial or color(255,255,255,255); return item
end
function ui_group_mt:texture(...) return make_item(self,'texture','Texture') end
function ui_item_mt:get(option)
  if self._override~=nil then return self._override end
  if self._type=='color_picker' then return self._color end
  if option==nil then return ui_native._get(self._id) end
  return ui_native._get(self._id,option)
end
function ui_item_mt:set(value,...) if self._type=='color_picker' then self._color=value else ui_native._set(self._id,value,...) end; return self end
function ui_item_mt:id() return self._id end
function ui_item_mt:list() return self._items end
function ui_item_mt:type() return self._type end
function ui_item_mt:override(value,...) self._override=value; return self end
function ui_item_mt:get_override() return self._override end
function ui_item_mt:update(...)
  local values=option_list(...); self._items=values; return self
end
function ui_item_mt:reset() self._override=nil; return self end
function ui_item_mt:name(value) if value~=nil then self._name=tostring(value) end return self._name end
function ui_item_mt:tooltip(value) if value~=nil then self._tooltip=value end return self._tooltip end
function ui_item_mt:visibility(value) if value==nil then return ui_native._state(self._id,'visible') end return ui_native._state(self._id,'visible',value) end
function ui_item_mt:disabled(value) if value==nil then return ui_native._state(self._id,'disabled') end return ui_native._state(self._id,'disabled',value) end
function ui_item_mt:set_callback(callback,force_call)
  self._callback=callback
  self._callback_wrapper=callback and function() callback(self) end or nil
  ui_native._set_callback(self._id,self._callback_wrapper)
  if force_call and callback then callback(self) end
  return self
end
function ui_item_mt:unset_callback(callback)
  if callback==nil or callback==self._callback then
    self._callback,self._callback_wrapper=nil,nil
    ui_native._set_callback(self._id,nil)
  end
  return self
end
function ui.find(...) local args={...}; return ui_registry[tostring(args[#args])] end
function ui.get_alpha() return 1 end
function ui.get_size() return vector(620,430,0) end
function ui.get_position() return vector(0,0,0) end
function ui.get_mouse_position() return vector(0,0,0) end
function ui.get_binds()
  local binds={}
  for _,reference in ipairs(ui_hotkeys) do
    local key,mode,active=ui_native._hotkey_info(reference._id)
    if key~=nil then
      binds[#binds+1]={name=reference._name,mode=mode,value=key,
        active=active,reference=reference}
    end
  end
  return binds
end
function ui.get_style(name) local styles={accent=color(148,72,255,255)}; return name and styles[name] or styles end
function ui.get_icon(name) return name=='gear' and '⚙' or tostring(name or '') end
function ui.sidebar(name,icon) return name,icon end
function ui.localize(lang,text,localized)
  ui_locales[lang]=ui_locales[lang] or {}; if localized~=nil then ui_locales[lang][text]=localized; return end
  return ui_locales[lang][text] or text
end
function utf8.codes(text)
  local position, length = 1, #text
  return function()
    if position>length then return nil end
    local start, a = position, text:byte(position); local code, count
    if a<0x80 then code,count=a,1
    elseif a<0xe0 then code,count=a%0x20,2
    elseif a<0xf0 then code,count=a%0x10,3
    else code,count=a%0x08,4 end
    for offset=2,count do
      local b=text:byte(position+offset-1)
      if not b or b<0x80 or b>=0xc0 then error('invalid UTF-8 sequence at byte '..start) end
      code=code*0x40+(b%0x40)
    end
    position=position+count
    return start,code
  end
end

local function clamp(v, a, b) return math.max(a, math.min(b, v)) end

local vec_mt = {}
vec_mt.__index = vec_mt
function vector(x, y, z) return setmetatable({x=x or 0, y=y or 0, z=z or 0}, vec_mt) end
function vec_mt:clone() return vector(self.x,self.y,self.z) end
function vec_mt:unpack() return self.x,self.y,self.z end
function vec_mt:length() return math.sqrt(self.x*self.x+self.y*self.y+self.z*self.z) end
function vec_mt:length2d() return math.sqrt(self.x*self.x+self.y*self.y) end
function vec_mt:length_sqr() return self.x*self.x+self.y*self.y+self.z*self.z end
function vec_mt:dist(other) return (self-other):length() end
function vec_mt:dist_sqr(other) return (self-other):length_sqr() end
function vec_mt:dot(other) return self.x*other.x+self.y*other.y+self.z*other.z end
function vec_mt:cross(other) return vector(self.y*other.z-self.z*other.y,self.z*other.x-self.x*other.z,self.x*other.y-self.y*other.x) end
function vec_mt:normalized() local l=self:length(); return l==0 and vector() or self/l end
function vec_mt:normalize() local n=self:normalized(); self.x,self.y,self.z=n.x,n.y,n.z; return self end
function vec_mt:lerp(other,t) return self+(other-self)*t end
function vec_mt:angles() return vector(math.deg(math.atan(-self.z,self:length2d())),math.deg(math.atan(self.y,self.x)),0) end
vec_mt.__add=function(a,b) return vector(a.x+b.x,a.y+b.y,a.z+b.z) end
vec_mt.__sub=function(a,b) return vector(a.x-b.x,a.y-b.y,a.z-b.z) end
vec_mt.__unm=function(a) return vector(-a.x,-a.y,-a.z) end
vec_mt.__mul=function(a,b) if type(a)=='number' then return vector(a*b.x,a*b.y,a*b.z) elseif type(b)=='number' then return vector(a.x*b,a.y*b,a.z*b) end return vector(a.x*b.x,a.y*b.y,a.z*b.z) end
vec_mt.__div=function(a,b) return type(b)=='number' and vector(a.x/b,a.y/b,a.z/b) or vector(a.x/b.x,a.y/b.y,a.z/b.z) end
vec_mt.__eq=function(a,b) return a.x==b.x and a.y==b.y and a.z==b.z end
vec_mt.__tostring=function(v) return ('vector(%.3f, %.3f, %.3f)'):format(v.x,v.y,v.z) end

local color_mt = {}; color_mt.__index=color_mt
function color(r,g,b,a)
  if type(r)=='string' then local s=r:gsub('#',''); if #s==2 then local v=tonumber(s,16); r,g,b,a=v,v,v,255 elseif #s==6 or #s==8 then r,g,b,a=tonumber(s:sub(1,2),16),tonumber(s:sub(3,4),16),tonumber(s:sub(5,6),16),#s==8 and tonumber(s:sub(7,8),16) or 255 end end
  if g==nil then g,b=r,r end; return setmetatable({r=r or 255,g=g or 255,b=b or 255,a=a or 255},color_mt)
end
function color_mt:clone() return color(self.r,self.g,self.b,self.a) end
function color_mt:init(r,g,b,a) local c=color(r,g,b,a); self.r,self.g,self.b,self.a=c.r,c.g,c.b,c.a; return self end
function color_mt:unpack() return self.r,self.g,self.b,self.a end
function color_mt:to_fraction() return self.r/255,self.g/255,self.b/255,self.a/255 end
function color_mt:to_hex() return ('%02X%02X%02X%02X'):format(self.r,self.g,self.b,self.a) end
function color_mt:to_int32() return self.r+self.g*256+self.b*65536+self.a*16777216 end
function color_mt:lerp(o,t) return color(self.r+(o.r-self.r)*t,self.g+(o.g-self.g)*t,self.b+(o.b-self.b)*t,self.a+(o.a-self.a)*t) end
function color_mt:grayscale(w) local y=.299*self.r+.587*self.g+.114*self.b; return self:lerp(color(y,y,y,self.a),w or 1) end
function color_mt:alpha_modulate(a) return color(self.r,self.g,self.b,a) end
color_mt.__tostring=function(c) return ('color(%d,%d,%d,%d)'):format(c.r,c.g,c.b,c.a) end

globals = globals or {}
local started, last = os.clock(), os.clock()
setmetatable(globals,{__index=function(_,k)
  local now=os.clock(); local map={realtime=now-started,curtime=now-started,frametime=now-last,absoluteframetime=now-last,
    tickcount=math.floor((now-started)/0.015625),tickinterval=0.015625,framecount=math.floor((now-started)*60),max_players=64}
  last=now; return map[k]
end})

utils = utils or {}
function utils.random_int(a,b) return math.random(a,b) end
function utils.random_float(a,b) return a+(b-a)*math.random() end
function utils.random_seed(seed) math.randomseed(seed) end
function utils.get_unix_time() return os.time() end
function utils.get_timestamp() return os.time() end
function utils.clamp(v,a,b) return clamp(v,a,b) end
function utils.lerp(a,b,t) return a+(b-a)*t end
local delayed={}
function utils.execute_after(delay, callback, ...)
  assert(type(callback)=='function','callback must be a function')
  delayed[#delayed+1]={at=os.clock()+(delay or 0),fn=callback,args={...},owner=rawget(_G,'__cs2lua_active_script')}
end
function __nl_process_timers()
  local now=os.clock()
  for i=#delayed,1,-1 do
    local job=delayed[i]
    if now>=job.at then
      table.remove(delayed,i)
      local previous=rawget(_G,'__cs2lua_active_script')
      rawset(_G,'__cs2lua_active_script',job.owner)
      local ok,err=pcall(job.fn,table.unpack(job.args))
      rawset(_G,'__cs2lua_active_script',previous)
      if not ok then error(err,0) end
    end
  end
end
function __nl_cancel_script_timers(owner)
  for i=#delayed,1,-1 do if delayed[i].owner==owner then table.remove(delayed,i) end end
end
local function empty_trace(from,to)
  local tr={start_pos=from,end_pos=to,fraction=1,contents=0,disp_flags=0,all_solid=false,start_solid=false,
    fraction_left_solid=0,surface={name='',props=0,flags=0},hitgroup=0,physics_bone=0,world_surface_index=0,entity=nil,hitbox=-1}
  function tr:did_hit() return self.fraction<1 or self.all_solid or self.start_solid end
  function tr:did_hit_world() return false end
  function tr:did_hit_non_world() return self:did_hit() and self.entity~=nil end
  function tr:is_visible() return self.fraction>0.97 end
  return tr
end
function utils.trace_line(from,to,skip,mask,trace_type) return empty_trace(from,to) end
function utils.trace_hull(from,to,mins,maxs,skip,mask,trace_type) return empty_trace(from,to) end
function utils.trace_bullet(from_entity,from,to,skip) return 0,empty_trace(from,to) end
function utils.get_netvar_offset(tbl,prop)
  if offsets and offsets.offsets then
    for _,module in pairs(offsets.offsets) do if module[prop] then return module[prop] end end
  end
  return nil
end

-- utils.get_vfunc(index, ctype)
-- utils.get_vfunc(module, interface, index, ctype)
-- The returned wrapper either expects the object pointer as its first argument,
-- or is already bound to the interface pointer in the module/interface overload.
function utils.get_vfunc(a, b, c, ...)
  local native_ffi = ffi or require('ffi')
  if type(a) == 'number' then
    local index, ctype = a, b
    assert(type(ctype) == 'string', 'utils.get_vfunc: FFI C function type expected')
    return function(instance, ...)
      assert(instance ~= nil, 'utils.get_vfunc: instance pointer expected')
      local vtable = native_ffi.cast('void***', instance)[0]
      local address = vtable[index]
      assert(address ~= nil, 'utils.get_vfunc: null virtual function')
      return native_ffi.cast(ctype, address)(instance, ...)
    end
  end

  assert(type(a) == 'string' and type(b) == 'string' and type(c) == 'number',
    'utils.get_vfunc: expected (module, interface, index, ctype)')
  local module_name, interface_name, index = a, b, c
  local ctype = select(1, ...)
  assert(type(ctype) == 'string', 'utils.get_vfunc: FFI C function type expected')
  local instance = utils.create_interface(module_name, interface_name)
  if instance == nil then return nil end
  local unbound = utils.get_vfunc(index, ctype)
  return function(...) return unbound(instance, ...) end
end
local net_stub={time=0,time_connected=0,time_since_last_received=0,is_loopback=true,is_playback=false,is_timing_out=false,
 sequence_nr={[0]=0,[1]=0},latency={[0]=0,[1]=0},avg_latency={[0]=0,[1]=0},loss={[0]=0,[1]=0},choke={[0]=0,[1]=0},
 packets={[0]=0,[1]=0},data={[0]=0,[1]=0},total_packets={[0]=0,[1]=0},total_data={[0]=0,[1]=0}}
function net_stub:get_server_info() return nil end
function net_stub:is_valid_packet() return false end
function net_stub:get_packet_time() return 0 end
function net_stub:get_packet_bytes() return 0 end
function net_stub:get_packet_response_latency() return 0,0 end
function utils.net_channel() return net_stub end

common = common or {}
function common.get_username() return os.getenv('USERNAME') or 'player' end
function common.get_product_version() return 'CS2LuaPlugin 0.2' end
function common.get_date(format, unix_time) return os.date(format or '%c', unix_time) end
function common.get_unixtime() return os.time() end
function common.get_timestamp() return os.time()*1000+math.floor((os.clock()%1)*1000) end
function common.get_system_time()
  local t=os.date('*t'); return {year=t.year,month=t.month,day=t.day,hours=t.hour,minutes=t.min,seconds=t.sec}
end
function common.get_game_directory() return '.' end
function common.get_map_data() return {name='',shortname='',group=''} end
function common.get_config_name() return 'default' end
function common.get_active_scripts() return {} end
function common.is_in_thirdperson() return false end
function common.add_event(text,icon_name) print('[event] '..tostring(text)) end
function common.add_notify(title,text) print(('[%s] %s'):format(tostring(title),tostring(text))) end

db = db or {}; local db_store={}
function db.read(k) return db_store[k] end
function db.write(k,v) db_store[k]=v; return true end
function db.delete(k) db_store[k]=nil end

files = files or {}
files.read = files.read or function(path) local f=io.open(path,'rb'); if not f then return nil end local d=f:read('*a'); f:close(); return d end
files.write = files.write or function(path,data) local f=assert(io.open(path,'wb')); f:write(data); f:close(); return true end
files.append = files.append or function(path,data) local f=assert(io.open(path,'ab')); f:write(data); f:close(); return true end
files.exists = files.exists or function(path) local f=io.open(path,'rb'); if f then f:close(); return true end return false end

-- LuaJIT provides the native BitOp-compatible `bit` module.
bit = bit or require('bit')

for _,name in ipairs({'cvar','esp','json','materials','ui','network','panorama','rage','render'}) do
  _G[name]=_G[name] or {}
end

schema = schema or {}
function schema.get_offset(module_name, field)
  local root = offsets and offsets.offsets
  return root and root[module_name] and root[module_name][field] or nil
end
function schema.get_button(name)
  local root = offsets and offsets.buttons
  return root and root['client.dll'] and root['client.dll'][name] or nil
end
function schema.get_build_number() return offsets and offsets.info and offsets.info.build_number or nil end

local function json_escape(s) return s:gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n'):gsub('\r','\\r'):gsub('\t','\\t') end
local function json_encode(v, seen)
  local t=type(v)
  if t=='nil' then return 'null' elseif t=='boolean' or t=='number' then return tostring(v) elseif t=='string' then return '"'..json_escape(v)..'"' end
  if t~='table' then error('json.stringify: unsupported type '..t) end
  seen=seen or {}; if seen[v] then error('json.stringify: circular table') end; seen[v]=true
  local array,max=true,0; for k in pairs(v) do if type(k)~='number' then array=false; break end max=math.max(max,k) end
  local out={}; if array then for i=1,max do out[#out+1]=json_encode(v[i],seen) end seen[v]=nil; return '['..table.concat(out,',')..']' end
  for k,val in pairs(v) do out[#out+1]=json_encode(tostring(k),seen)..':'..json_encode(val,seen) end
  seen[v]=nil; return '{'..table.concat(out,',')..'}'
end
function json.stringify(value) return json_encode(value) end
json.encode=json.stringify
local function json_decode(text)
  local i,n=1,#text
  local function ws() while i<=n and text:sub(i,i):match('%s') do i=i+1 end end
  local parse
  local function string_value()
    i=i+1; local out={}
    while i<=n do local c=text:sub(i,i); i=i+1
      if c=='"' then return table.concat(out) end
      if c=='\\' then
        local e=text:sub(i,i); i=i+1
        local map={['"']='"',['\\']='\\',['/']='/',b='\b',f='\f',n='\n',r='\r',t='\t'}
        if e=='u' then
          local hex=text:sub(i,i+3); if not hex:match('^%x%x%x%x$') then error('json.parse: invalid unicode escape at '..i) end
          i=i+4; local cp=tonumber(hex,16)
          if cp>=0xD800 and cp<=0xDBFF and text:sub(i,i+1)=='\\u' then
            local low=tonumber(text:sub(i+2,i+5),16)
            if low and low>=0xDC00 and low<=0xDFFF then cp=0x10000+(cp-0xD800)*0x400+(low-0xDC00); i=i+6 end
          end
          out[#out+1]=utf8.char(cp)
        else out[#out+1]=map[e] or e end
      else out[#out+1]=c end
    end
    error('json.parse: unterminated string')
  end
  local function array_value()
    i=i+1; local out={}; ws(); if text:sub(i,i)==']' then i=i+1 return out end
    while true do out[#out+1]=parse(); ws(); local c=text:sub(i,i); i=i+1; if c==']' then return out elseif c~=',' then error('json.parse: expected , or ] at '..(i-1)) end end
  end
  local function object_value()
    i=i+1; local out={}; ws(); if text:sub(i,i)=='}' then i=i+1 return out end
    while true do ws(); if text:sub(i,i)~='"' then error('json.parse: expected key at '..i) end local k=string_value(); ws(); if text:sub(i,i)~=':' then error('json.parse: expected : at '..i) end i=i+1; out[k]=parse(); ws(); local c=text:sub(i,i); i=i+1; if c=='}' then return out elseif c~=',' then error('json.parse: expected , or } at '..(i-1)) end end
  end
  function parse()
    ws(); local c=text:sub(i,i)
    if c=='"' then return string_value() elseif c=='{' then return object_value() elseif c=='[' then return array_value() end
    local tail=text:sub(i)
    if tail:sub(1,4)=='true' then i=i+4 return true elseif tail:sub(1,5)=='false' then i=i+5 return false elseif tail:sub(1,4)=='null' then i=i+4 return nil end
    local s,e=tail:find('^-?%d+%.?%d*[eE]?[+-]?%d*'); if s then local num=tonumber(tail:sub(s,e)); i=i+e; return num end
    error('json.parse: invalid value at '..i)
  end
  local value=parse(); ws(); if i<=n then error('json.parse: trailing data at '..i) end return value
end
function json.parse(text) assert(type(text)=='string','json.parse expects string'); return json_decode(text) end
json.decode=json.parse

-- Persistent db storage. Values supported by json are restored on next load.
local db_file='.cs2lua_db.json'
do
  local raw=files.read(db_file)
  if raw then local ok,value=pcall(json.decode,raw); if ok and type(value)=='table' then db_store=value end end
  local function save_db() local ok,data=pcall(json.encode,db_store); if ok then files.write(db_file,data) end end
  function db.read(k) return db_store[k] end
  function db.write(k,v) db_store[k]=v; save_db(); return true end
  function db.delete(k) db_store[k]=nil; save_db(); return true end
end

