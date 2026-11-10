-- To-do: lists of rows, the keyboard, ui.choose and table storage.
local items = storage.get("items", {})            -- {{text = "...", done = false}, ...}
local list

local function save() storage.set("items", items) end

local function render()
  list:clear()
  ui.button(ui.icon("plus") .. " New item", function()
    ui.keyboard("New item", "", function(text)
      if text ~= "" then
        table.insert(items, {text = text, done = false})
        save()
        render()
      end
    end)
  end, {parent = list, w = "100%"})

  if #items == 0 then
    ui.label("Nothing to do!", {parent = list, color = 0x8E8E93, font = 20})
  end
  for i, item in ipairs(items) do
    ui.row(item.text, {
      parent = list,
      icon = item.done and "ok" or "minus",
      on_click = function()
        ui.choose(item.text, {item.done and "Mark not done" or "Mark done", "Delete"}, function(choice)
          if choice == 1 then item.done = not item.done else table.remove(items, i) end
          save()
          render()
        end)
      end,
    })
  end
end

list = ui.list()
render()
