local env1 = environment()
local env2 = environment()
env1:add_var("N", Type())
env1:add_var("x", Const("N"))
env2:add_var("N", Type())
local x = Const("x")
local N = Const("N")
print(env1:infer_type(x))
local infer1 = type_inferer(env1)
local infer2 = type_inferer(env2)
assert(env1:infer_type(x) == infer1(x))
assert(env2:infer_type(N) == infer2(N))
