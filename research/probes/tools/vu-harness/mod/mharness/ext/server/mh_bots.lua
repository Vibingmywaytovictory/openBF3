-- Test-bot fixtures: the ONLY module that writes game state (SKELETON; DESIGN.md section 3.9; R section 3.2 rule 3).
-- Every fixture emits an 'fx' row and refuses players it did not create.
-- PlayerManager:CreatePlayer/DeletePlayer (server/library/playermanager.md); Player:SelectWeapon/SelectUnlockAssets/
-- CreateSoldier/SpawnSoldierAt/EnterVehicle (server/type/player.md); SoldierEntity:SetPosition/SetPose/Kill
-- (server/type/soldierentity.md); EntryInput (shared/type/entryinput.md, shared/type/entryinputflags.md);
-- ResourceManager:SearchForDataContainer (shared/library/resourcemanager.md, read only).
local M = {}

function M.create(team, kit, unlock) return nil end
function M.spawn(bot, pos, yaw, pose) end
function M.place(bot, pos, yaw) end
function M.pose(bot, pose) end
function M.aim(bot, yaw, pitch) end
function M.hold(bot, action, level) end
function M.release_all(bot) end
-- apply_inputs(dt): every input pass sets input.deltaTime = dt (1/GetTickrate) before assigning player.input (DESIGN 3.9).
function M.apply_inputs(dt) end
-- fire_mode(bot, want): reads SoldierWeapon.fireLogic (RO use only) and cycles with EIACycleFireMode until it matches.
function M.fire_mode(bot, want) end
-- select_slot(bot, slot): EIASelectWeapon<n> input, verified by weaponsComponent.currentWeaponSlot.
function M.select_slot(bot, slot) end
function M.kill(bot) end
function M.remove(bot) end
function M.remove_all() end
function M.seat(bot, vehicle, entryId) end -- Sessions 2-3

return M
