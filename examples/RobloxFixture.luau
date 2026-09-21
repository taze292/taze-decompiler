local UserInputService = game:GetService("UserInputService")
local RunService = game:GetService("RunService")

local function ServiceNames()
    return UserInputService.Name, RunService.Name
end

local function MakeCounter(Start)
    local Count = Start

    return function(Amount)
        Count += Amount
        return Count
    end
end

local function SumValues(...)
    local Values = table.pack(...)
    local Sum = 0

    for Index = 1, Values.n do
        local Value = Values[Index]
        if Value then
            Sum += Value
        end
    end

    return Sum, Values.n, ...
end

local Counter = MakeCounter(10)
local First, Second = Counter(3), Counter(5)
local InputName, RunName = ServiceNames()

return InputName, RunName, First, Second, SumValues(2, nil, 7, nil)
