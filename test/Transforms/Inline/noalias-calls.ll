; RUN: opt -inline -enable-noalias-to-md-conversion -S < %s | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @llvm.memcpy.p0i8.p0i8.i64(i8* nocapture, i8* nocapture readonly, i64, i32, i1) #0
declare void @hey() #0

define void @hello(i8* noalias nocapture %a, i8* noalias nocapture readonly %c, i8* nocapture %b) #1 {
entry:
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %a, i8* %b, i64 16, i32 16, i1 0)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %b, i8* %c, i64 16, i32 16, i1 0)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %a, i8* %c, i64 16, i32 16, i1 0)
  call void @hey()
  ret void
}

define void @foo(i8* nocapture %a, i8* nocapture readonly %c, i8* nocapture %b) #1 {
entry:
  tail call void @hello(i8* %a, i8* %c, i8* %b)
  ret void
}

; CHECK: define void @foo(i8* nocapture %a, i8* nocapture readonly %c, i8* nocapture %b) #1 {
; CHECK: entry:
; CHECK:   call void @llvm.memcpy.p0i8.p0i8.i64(i8* %a, i8* %b, i64 16, i32 16, i1 false) #0, !noalias !0
; CHECK:   call void @llvm.memcpy.p0i8.p0i8.i64(i8* %b, i8* %c, i64 16, i32 16, i1 false) #0, !noalias !3
; CHECK:   call void @llvm.memcpy.p0i8.p0i8.i64(i8* %a, i8* %c, i64 16, i32 16, i1 false) #0, !alias.scope !5
; CHECK:   call void @hey() #0, !noalias !5
; CHECK:   ret void
; CHECK: }

attributes #0 = { nounwind }
attributes #1 = { nounwind uwtable }

; CHECK: !0 = metadata !{metadata !1}
; CHECK: !1 = metadata !{metadata !1, metadata !2, metadata !"hello: %c"}
; CHECK: !2 = metadata !{metadata !2, metadata !"hello"}
; CHECK: !3 = metadata !{metadata !4}
; CHECK: !4 = metadata !{metadata !4, metadata !2, metadata !"hello: %a"}
; CHECK: !5 = metadata !{metadata !4, metadata !1}

