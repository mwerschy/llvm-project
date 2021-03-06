; RUN: llc -march=amdgcn -verify-machineinstrs < %s | FileCheck -check-prefix=SI %s
; RUN: llc -march=amdgcn -mcpu=tonga -verify-machineinstrs < %s | FileCheck -check-prefix=SI %s

; SI-LABEL: {{^}}br_i1_phi:

; SI: ; %bb
; SI:    s_mov_b64           [[TMP:s\[[0-9]+:[0-9]+\]]], 0

; SI: ; %bb2
; SI:    s_mov_b64           [[TMP]], exec

; SI: ; %bb3
; SI:    s_and_saveexec_b64  {{s\[[0-9]+:[0-9]+\]}}, [[TMP]]

define amdgpu_kernel void @br_i1_phi(i32 %arg) {
bb:
  %tidig = call i32 @llvm.amdgcn.workitem.id.x()
  %cmp = trunc i32 %tidig to i1
  br i1 %cmp, label %bb2, label %bb3

bb2:                                              ; preds = %bb
  br label %bb3

bb3:                                              ; preds = %bb2, %bb
  %tmp = phi i1 [ true, %bb2 ], [ false, %bb ]
  br i1 %tmp, label %bb4, label %bb6

bb4:                                              ; preds = %bb3
  %val = load volatile i32, i32 addrspace(1)* undef
  %tmp5 = mul i32 %val, %arg
  br label %bb6

bb6:                                              ; preds = %bb4, %bb3
  ret void
}

declare i32 @llvm.amdgcn.workitem.id.x() #0

attributes #0 = { nounwind readnone }
