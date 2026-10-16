-- Pass-through hooks (SKELETON; DESIGN.md section 3.10). Never Pass, never Return.
-- EntityFactory:Create (server/hook/entityfactory_create.md): Call() only for projectile data types -> 'shot'.
-- BulletEntity:Collision (server/hook/bulletentity_collision.md): read only -> 'hit'.
-- Soldier:Damage (server/hook/soldier_damage.md): 'dmg' pre, Call(), 'dmg' post with soldier.health.
local M = {}

function M.install() end

return M
