print('========== entity 原生桥接测试开始 ==========', CS2LUA_API_VERSION)
local status=entity.debug_status()
print(('[诊断] client=%s entity_list=%s controller=%s pawn=%s rules=%s offsets_build=%s'):format(
  tostring(status.client_base),tostring(status.entity_list),tostring(status.local_controller),
  tostring(status.local_pawn),tostring(status.game_rules),tostring(status.build_number)))
print(('[扫描] entity_global=%s controller_global=%s rules_global=%s'):format(
  tostring(status.scan_entity_list),tostring(status.scan_local_controller),tostring(status.scan_game_rules)))
print(('[布局] stride=%s pointer_offset=%s validated=%s'):format(
  tostring(status.entity_stride),tostring(status.entity_pointer_offset),tostring(status.entity_layout_validated)))
print(('[Pawn] handle=%s index=%s pointer=%s controller_hp=%s armor=%s pawn_hp=%s team=%s scene=%s'):format(
  tostring(status.pawn_handle),tostring(status.pawn_index),tostring(status.resolved_pawn),
  tostring(status.controller_health),tostring(status.controller_armor),tostring(status.pawn_health),
  tostring(status.pawn_team),tostring(status.scene_node)))
if not status.resolved_pawn or status.resolved_pawn=='nil' then
  print('[等待] 当前尚未建立本地玩家 Pawn；请进入本地地图后 Reload 此测试脚本')
end
print(('[武器] services=%s active_handle=%s'):format(tostring(status.weapon_services),tostring(status.weapon_handle)))
local passed,failed=0,0
local function test(name,fn)
  local ok,result=pcall(fn)
  if ok and result~=false then passed=passed+1; print('[成功] '..name..' 功能正常')
  else failed=failed+1; print('[失败] '..name..' 功能异常: '..tostring(result)) end
end

test('entity.get_local_player',function()
  local me=assert(entity.get_local_player(),'请先进入本地地图')
  assert(me[0]~=nil and me:is_player())
  print('local index=',me:get_index(),'health=',me.m_iHealth,'team=',me.m_iTeamNum)
end)
test('entity player methods',function()
  local me=assert(entity.get_local_player())
  local origin,angles,eyes=me:get_origin(),me:get_angles(),me:get_eye_position()
  assert(origin and angles and eyes and type(me:is_alive())=='boolean')
  print('origin=',origin.x,origin.y,origin.z,'alive=',me:is_alive())
end)
test('entity.get_players',function()
  local players=entity.get_players(false,true)
  assert(type(players)=='table')
  print('players=',#players)
  for _,player in ipairs(players) do
    print(player:get_index(),player:get_name(),player.m_iHealth,player:is_enemy(),player:is_dormant())
  end
end)
test('entity callback iteration',function()
  local count=0; entity.get_players(false,true,function(player) count=count+1 end)
  assert(count>=0); print('callback players=',count)
end)
test('entity active weapon',function()
  local me=assert(entity.get_local_player()); local weapon=me:get_player_weapon()
  if weapon then
    print('weapon index=',weapon:get_weapon_index(),'pointer=',tostring(weapon[0]))
    assert(weapon:get_weapon_index()>0,'活动武器 handle 解析出的 index 无效')
  else print('当前没有活动武器') end
end)
test('entity.get_game_rules',function()
  local rules=entity.get_game_rules(); assert(rules~=nil,'当前地图没有 GameRules')
end)
print(('========== entity 测试完成：成功 %d，失败 %d =========='):format(passed,failed))
