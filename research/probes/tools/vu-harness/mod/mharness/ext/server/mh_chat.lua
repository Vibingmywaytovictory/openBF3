-- Chat replies and prompts (DESIGN.md sections 3.7, 4 'prompt').
-- ChatManager:SendMessage(message, toPlayer) -> bool and SendMessage(message) to everyone; Yell(message, duration,
-- toPlayer) (server/library/chatmanager.md). The message limit is 127 characters: at most 120 per message including
-- the '[mh] ' prefix, at most 6 messages per reply. Every reply is also sent to the client over NetEvents 'mh:say'
-- (server/library/netevents.md), which prints it in the client log/console (the console fallback path, L06/L19).
-- Every instruction to the user is also a 'prompt' row (machine-readable for the extractor and the mock user).
local R = require('__shared/mh_row')
local ids = require('mh_ids')

local M = {}

M.PREFIX = '[mh] '
M.MAX = 120
M.MAX_MSGS = 6
M.sent = 0

local function chunks(text)
	text = tostring(text):gsub('[%c]', ' ')
	local room = M.MAX - #M.PREFIX
	local out = {}
	while #text > 0 and #out < M.MAX_MSGS do
		if #text <= room then
			out[#out + 1] = text
			break
		end
		local cut = room
		local sp = text:sub(1, room):match('.*() ')
		if sp and sp > room // 2 then cut = sp - 1 end
		out[#out + 1] = text:sub(1, cut)
		text = text:sub(cut + 1):gsub('^ +', '')
	end
	return out
end
M.chunks = chunks

local function send_one(pl, msg)
	M.sent = M.sent + 1
	if pl ~= nil then
		pcall(function() ChatManager:SendMessage(msg, pl) end)
		pcall(function() NetEvents:SendTo('mh:say', pl, msg) end)
	else
		pcall(function() ChatManager:SendMessage(msg) end)
		pcall(function() NetEvents:Broadcast('mh:say', msg) end)
	end
end

-- say(player|nil, text): nil sends to every human.
function M.say(pl, text)
	for _, c in ipairs(chunks(text)) do send_one(pl, M.PREFIX .. c) end
end

function M.yell(pl, text, seconds)
	local msg = M.PREFIX .. chunks(text)[1]
	if pl ~= nil then
		pcall(function() ChatManager:Yell(msg, seconds or 3.0, pl) end)
	else
		pcall(function() ChatManager:Yell(msg, seconds or 3.0) end)
	end
end

M.last_progress = {}

-- progress(pl, key, text, now_s): at most one chat line per key every 30 s.
function M.progress(pl, key, text, now_s)
	local last = M.last_progress[key]
	if last and now_s - last < 30 then return false end
	M.last_progress[key] = now_s
	M.say(pl, text)
	return true
end

-- prompt(player, kind, fields, text): chat line + prompt row {kind, n, of, wid, zone, d, id, dir, wait}.
function M.prompt(pl, kind, fields, text)
	R.row('prompt', { kind = kind, n = fields.n or 1, of = fields.of or 1, wid = fields.wid, zone = fields.zone,
		d = fields.d, id = fields.tid, dir = fields.dir, wait = fields.wait })
	M.say(pl, text)
end

function M.all_humans()
	return ids.human_list()
end

return M
