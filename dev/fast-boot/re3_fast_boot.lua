-- RE3R fast-boot for debug iteration.
-- Stops any live via.movie.MovieController each frame, which collapses the
-- Capcom / RE Engine / publisher logo chain at launch and any pre-rendered
-- cutscenes triggered while this script is active.
--
-- Drop into <RE3>/reframework/autorun/. Remove the file to restore normal
-- behaviour. Development only - do not ship with the mod.

local MOVIE_T = sdk.find_type_definition("via.movie.MovieController")
if MOVIE_T == nil then
    error("re3_fast_boot: via.movie.MovieController not found - RE Engine API changed")
end

local m_stop = MOVIE_T:get_method("stop")
if m_stop == nil then
    error("re3_fast_boot: via.movie.MovieController.stop() not found")
end

local stopped_count = 0

re.on_frame(function()
    local controllers = sdk.find_components_by_type("via.movie.MovieController")
    if controllers == nil then return end
    for _, mc in ipairs(controllers) do
        m_stop:call(mc)
        stopped_count = stopped_count + 1
    end
end)

re.on_draw_ui(function()
    imgui.text(string.format("[fast-boot] stop() calls: %d", stopped_count))
end)
