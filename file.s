  .section .note.GNU-stack,"",@progbits
  .data
  .globl g
g:
  .long 1
  .long 2
  .long 3
  .text
  .globl main
main:
  pushq %rbp
  movq %rsp, %rbp
  subq $16, %rsp
  movq $0, %rax
  pushq %rax
  leaq -4(%rbp), %rax
  popq %rcx
  movl %ecx, (%rax)
  movq %rcx, %rax
.Lfor0:
  movq $3, %rax
  pushq %rax
  leaq -4(%rbp), %rax
  movslq (%rax), %rax
  popq %rcx
  cmpq %rcx, %rax
  setl %al
  movzbq %al, %rax
  cmpq $0, %rax
  je .Lend0
  leaq g(%rip), %rax
  pushq %rax
  leaq -4(%rbp), %rax
  movslq (%rax), %rax
  imulq $4, %rax
  popq %rcx
  addq %rcx, %rax
  movslq (%rax), %rax
  pushq %rax
  .section .rodata
.LC1:
  .string "%d\n"
  .text
  leaq .LC1(%rip), %rax
  pushq %rax
  popq %rdi
  popq %rsi
  movb $0, %al
  callq printf
.Lstep0:
  leaq -4(%rbp), %rax
  movslq (%rax), %rcx
  movq %rcx, %rdx
  addq $1, %rdx
  movl  %edx, (%rax)
  movq %rcx, %rax
  jmp .Lfor0
.Lend0:
.Lret_main:
  movq %rbp, %rsp
  popq %rbp
  ret
