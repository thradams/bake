(module
  (import "env" "printf_variadic" (func $__printf_variadic (param i64) (param i32) (param i64) (result i64)))
  (import "env" "malloc" (func $malloc))
  (import "env" "free" (func $free))
  (memory (export "memory") 32)
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
        ;; variadic call: allocate va_buf (1 slots)
        global.get $__sp
        i32.const 8
        i32.sub
        global.set $__sp
        global.get $__sp
        i32.const 4096
        local.get 1
        i32.wrap_i64
        i32.const 4
        i32.mul
        i32.add
        i64.load32_s
        i64.store
        i64.const 1052672
        global.get $__sp
        i64.const 1
        call $__printf_variadic
        ;; free va_buf
        global.get $__sp
        i32.const 8
        i32.add
        global.set $__sp
        drop
        local.get 1
        local.get 1
        i64.const 1
        i64.add
        local.set 1
        drop
        br $loop_0
      end ;; loop_0
    end ;; brk_0
    ;; software stack epilogue
    local.get $__sp_saved
    global.set $__sp
    i64.const 0  ;; implicit return 0
  )
  (export "main" (func $main))
  
  (data (i32.const 1052672) "%d\0a\00")
  (data (i32.const 4096) "\01\00\00\00\02\00\00\00\03\00\00\00")
)
