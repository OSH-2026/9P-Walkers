-- P4 只负责调度和搬运；每个 tile 的像素全部由 STM32 job worker 计算。
local IMAGE_W <const> = 120
local IMAGE_H <const> = 160
local TILE_W <const> = 8
local TILE_H <const> = 7
local SAMPLES <const> = 2
local MAX_DEPTH <const> = 4

local function find_display(nodes)
    for _, node in ipairs(nodes) do
        if display.probe(node) then
            return node
        end
    end
    return nil
end

local function build_tiles(frame_id, phase)
    local tiles = {}
    for y = 0, IMAGE_H - 1, TILE_H do
        for x = 0, IMAGE_W - 1, TILE_W do
            local width = math.min(TILE_W, IMAGE_W - x)
            local height = math.min(TILE_H, IMAGE_H - y)
            local seed = (frame_id * 1103515245 + y * IMAGE_W + x + 12345) & 0x7fffffff
            tiles[#tiles + 1] = {
                x = x, y = y, width = width, height = height,
                frame = frame_id, phase = phase, seed = seed, attempts = 0,
            }
        end
    end
    return tiles
end

local function encode_tile(tile)
    return string.pack("<BBBBBBBBBBI2I4I2",
        1, 1, tile.x, tile.y, tile.width, tile.height,
        IMAGE_W, IMAGE_H, SAMPLES, MAX_DEPTH,
        tile.frame, tile.seed, tile.phase)
end

local function render_frame(nodes, display_node, frame_id, phase)
    local queue = build_tiles(frame_id, phase)
    local active = {}
    local completed = 0
    local total = #queue

    while completed < total do
        for _, node in ipairs(nodes) do
            if active[node] == nil and #queue > 0 then
                local tile = queue[#queue]
                queue[#queue] = nil
                local id, reason, code = job.submit(node, encode_tile(tile))
                if id ~= nil then
                    active[node] = {id = id, tile = tile}
                else
                    tile.attempts = tile.attempts + 1
                    queue[#queue + 1] = tile
                    host.log(string.format("submit %s failed %s(%s)",
                        node, tostring(reason), tostring(code)))
                end
            end
        end

        for node, work in pairs(active) do
            local pixels, state, progress = job.result(work.id)
            if pixels ~= nil then
                local ok, reason, code = display.blit(display_node, pixels)
                if ok then
                    completed = completed + 1
                else
                    work.tile.attempts = work.tile.attempts + 1
                    queue[#queue + 1] = work.tile
                    host.log(string.format("display write failed %s(%s)",
                        tostring(reason), tostring(code)))
                end
                active[node] = nil
            elseif state ~= "not_ready" then
                work.tile.attempts = work.tile.attempts + 1
                queue[#queue + 1] = work.tile
                active[node] = nil
                host.log(string.format("result %s failed %s(%s)",
                    node, tostring(state), tostring(progress)))
            end
        end
        host.sleep(25)
    end
    host.log(string.format("frame=%d complete tiles=%d workers=%d display=%s",
        frame_id, total, #nodes, display_node))
end

local frame_id = 1
local phase = 0
while true do
    local nodes = cluster.nodes()
    local display_node = find_display(nodes)
    if #nodes == 0 or display_node == nil then
        host.log("waiting for compute nodes and F429 display")
        host.sleep(1500)
    else
        render_frame(nodes, display_node, frame_id, phase)
        frame_id = (frame_id + 1) & 0xffff
        if frame_id == 0 then frame_id = 1 end
        phase = (phase + 4) % 360
    end
end
