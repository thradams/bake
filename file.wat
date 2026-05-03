(module
  (import "env" "malloc" (func $malloc))
  (import "env" "free" (func $free))
  (memory (export "memory") 16)
  (global $__sp (mut i32) (i32.const 1048576))
  ;; ----------------------------------------
  (func $main (result i64)
    (local $__sp_saved i32)
    (local i64) ;; offset -4
    ;; software stack prologue
    global.get $__sp
    local.set $__sp_saved
    global.get $__sp
    i32.const 16
    i32.sub
    global.set $__sp
    i64.const 0
    local.tee 1
    drop
    block $brk_0
      loop $loop_0
        local.get 1
        i64.const 3
        i64.lt_s
        i64.extend_i32_s
        i64.const 0
        i64.eq
        br_if $brk_0
