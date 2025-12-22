; REQUIRES: ortools
; RUN: llc -mtriple=x86_64-- -O2 -debug-pass=Structure -regalloc=regalloc-ilp %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ILP
; RUN: llc -mtriple=x86_64-- -O2 -debug-pass=Structure -regalloc=greedy %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=GREEDY

declare void @bar(i32)

define void @foo(i32 %a, i32 %b, i32 %c) nounwind {
; ILP: ILP Register Allocator
; ILP-NOT: Greedy Register Allocator
; GREEDY: Greedy Register Allocator
; GREEDY-NOT: ILP Register Allocator
entry:
  %add = add i32 %a, %b
  %mul = mul i32 %add, %c
  %sub = sub i32 %mul, %b
  call void @bar(i32 %sub)
  ret void
}
