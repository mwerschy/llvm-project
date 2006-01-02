//===-- AlphaISelDAGToDAG.cpp - Alpha pattern matching inst selector ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Andrew Lenharth and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a pattern matching instruction selector for Alpha,
// converting from a legalized dag to a Alpha dag.
//
//===----------------------------------------------------------------------===//

#include "Alpha.h"
#include "AlphaTargetMachine.h"
#include "AlphaISelLowering.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Constants.h"
#include "llvm/GlobalValue.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
using namespace llvm;

namespace {

  //===--------------------------------------------------------------------===//
  /// AlphaDAGToDAGISel - Alpha specific code to select Alpha machine
  /// instructions for SelectionDAG operations.
  class AlphaDAGToDAGISel : public SelectionDAGISel {
    AlphaTargetLowering AlphaLowering;

    static const int64_t IMM_LOW  = -32768;
    static const int64_t IMM_HIGH = 32767;
    static const int64_t IMM_MULT = 65536;
    static const int64_t IMM_FULLHIGH = IMM_HIGH + IMM_HIGH * IMM_MULT;
    static const int64_t IMM_FULLLOW = IMM_LOW + IMM_LOW  * IMM_MULT;

    static int64_t get_ldah16(int64_t x) {
      int64_t y = x / IMM_MULT;
      if (x % IMM_MULT > IMM_HIGH)
	++y;
      return y;
    }

    static int64_t get_lda16(int64_t x) {
      return x - get_ldah16(x) * IMM_MULT;
    }

    static uint64_t get_zapImm(uint64_t x) {
      unsigned int build = 0;
      for(int i = 0; i < 8; ++i)
	{
	  if ((x & 0x00FF) == 0x00FF)
	    build |= 1 << i;
	  else if ((x & 0x00FF) != 0)
	    { build = 0; break; }
	  x >>= 8;
	}
      return build;
    }

    static bool isFPZ(SDOperand N) {
      ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(N);
      return (CN && (CN->isExactlyValue(+0.0) || CN->isExactlyValue(-0.0)));
    }
    static bool isFPZn(SDOperand N) {
      ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(N);
      return (CN && CN->isExactlyValue(-0.0));
    }
    static bool isFPZp(SDOperand N) {
      ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(N);
      return (CN && CN->isExactlyValue(+0.0));
    }

  public:
    AlphaDAGToDAGISel(TargetMachine &TM)
      : SelectionDAGISel(AlphaLowering), AlphaLowering(TM) 
    {}

    /// getI64Imm - Return a target constant with the specified value, of type
    /// i64.
    inline SDOperand getI64Imm(int64_t Imm) {
      return CurDAG->getTargetConstant(Imm, MVT::i64);
    }

    // Select - Convert the specified operand from a target-independent to a
    // target-specific node if it hasn't already been changed.
    SDOperand Select(SDOperand Op);
    
    /// InstructionSelectBasicBlock - This callback is invoked by
    /// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
    virtual void InstructionSelectBasicBlock(SelectionDAG &DAG);
    
    virtual const char *getPassName() const {
      return "Alpha DAG->DAG Pattern Instruction Selection";
    } 

// Include the pieces autogenerated from the target description.
#include "AlphaGenDAGISel.inc"
    
private:
    SDOperand getGlobalBaseReg();
    SDOperand getRASaveReg();
    SDOperand SelectCALL(SDOperand Op);

  };
}

/// getGlobalBaseReg - Output the instructions required to put the
/// GOT address into a register.
///
SDOperand AlphaDAGToDAGISel::getGlobalBaseReg() {
  return CurDAG->getCopyFromReg(CurDAG->getEntryNode(), 
                                AlphaLowering.getVRegGP(), 
                                MVT::i64);
}

/// getRASaveReg - Grab the return address
///
SDOperand AlphaDAGToDAGISel::getRASaveReg() {
  return CurDAG->getCopyFromReg(CurDAG->getEntryNode(),
                                AlphaLowering.getVRegRA(), 
                                MVT::i64);
}

/// InstructionSelectBasicBlock - This callback is invoked by
/// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
void AlphaDAGToDAGISel::InstructionSelectBasicBlock(SelectionDAG &DAG) {
  DEBUG(BB->dump());
  
  // Select target instructions for the DAG.
  DAG.setRoot(Select(DAG.getRoot()));
  CodeGenMap.clear();
  DAG.RemoveDeadNodes();
  
  // Emit machine code to BB. 
  ScheduleAndEmitDAG(DAG);
}

// Select - Convert the specified operand from a target-independent to a
// target-specific node if it hasn't already been changed.
SDOperand AlphaDAGToDAGISel::Select(SDOperand Op) {
  SDNode *N = Op.Val;
  if (N->getOpcode() >= ISD::BUILTIN_OP_END &&
      N->getOpcode() < AlphaISD::FIRST_NUMBER)
    return Op;   // Already selected.

  // If this has already been converted, use it.
  std::map<SDOperand, SDOperand>::iterator CGMI = CodeGenMap.find(Op);
  if (CGMI != CodeGenMap.end()) return CGMI->second;
  
  switch (N->getOpcode()) {
  default: break;
  case ISD::TAILCALL:
  case ISD::CALL: return SelectCALL(Op);

  case ISD::DYNAMIC_STACKALLOC: {
    if (!isa<ConstantSDNode>(N->getOperand(2)) ||
        cast<ConstantSDNode>(N->getOperand(2))->getValue() != 0) {
      std::cerr << "Cannot allocate stack object with greater alignment than"
                << " the stack alignment yet!";
      abort();
    }

    SDOperand Chain = Select(N->getOperand(0));
    SDOperand Amt   = Select(N->getOperand(1));
    SDOperand Reg = CurDAG->getRegister(Alpha::R30, MVT::i64);
    SDOperand Val = CurDAG->getCopyFromReg(Chain, Alpha::R30, MVT::i64);
    Chain = Val.getValue(1);
    
    // Subtract the amount (guaranteed to be a multiple of the stack alignment)
    // from the stack pointer, giving us the result pointer.
    SDOperand Result = CurDAG->getTargetNode(Alpha::SUBQ, MVT::i64, Val, Amt);
    
    // Copy this result back into R30.
    Chain = CurDAG->getNode(ISD::CopyToReg, MVT::Other, Chain, Reg, Result);
    
    // Copy this result back out of R30 to make sure we're not using the stack
    // space without decrementing the stack pointer.
    Result = CurDAG->getCopyFromReg(Chain, Alpha::R30, MVT::i64);
  
    // Finally, replace the DYNAMIC_STACKALLOC with the copyfromreg.
    CodeGenMap[Op.getValue(0)] = Result;
    CodeGenMap[Op.getValue(1)] = Result.getValue(1);
    return SDOperand(Result.Val, Op.ResNo);
  }

  case ISD::FrameIndex: {
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    return CurDAG->SelectNodeTo(N, Alpha::LDA, MVT::i64,
                                CurDAG->getTargetFrameIndex(FI, MVT::i32),
                                getI64Imm(0));
  }
  case AlphaISD::GlobalBaseReg: 
    return getGlobalBaseReg();
  
  case AlphaISD::DivCall: {
    SDOperand Chain = CurDAG->getEntryNode();
    Chain = CurDAG->getCopyToReg(Chain, Alpha::R24, Select(Op.getOperand(1)), 
				 SDOperand(0,0));
    Chain = CurDAG->getCopyToReg(Chain, Alpha::R25, Select(Op.getOperand(2)), 
				 Chain.getValue(1));
    Chain = CurDAG->getCopyToReg(Chain, Alpha::R27, Select(Op.getOperand(0)), 
				 Chain.getValue(1));
    Chain = CurDAG->getTargetNode(Alpha::JSRs, MVT::Other, MVT::Flag, 
				  Chain, Chain.getValue(1));
    Chain = CurDAG->getCopyFromReg(Chain, Alpha::R27, MVT::i64, 
				  Chain.getValue(1));
    return CurDAG->SelectNodeTo(N, Alpha::BIS, MVT::i64, Chain, Chain);
  }

  case ISD::RET: {
    SDOperand Chain = Select(N->getOperand(0));     // Token chain.
    SDOperand InFlag;

    if (N->getNumOperands() == 2) {
      SDOperand Val = Select(N->getOperand(1));
      if (N->getOperand(1).getValueType() == MVT::i64) {
        Chain = CurDAG->getCopyToReg(Chain, Alpha::R0, Val, InFlag);
        InFlag = Chain.getValue(1);
      } else if (N->getOperand(1).getValueType() == MVT::f64 ||
                 N->getOperand(1).getValueType() == MVT::f32) {
        Chain = CurDAG->getCopyToReg(Chain, Alpha::F0, Val, InFlag);
        InFlag = Chain.getValue(1);
      }
    }
    Chain = CurDAG->getCopyToReg(Chain, Alpha::R26, getRASaveReg(), InFlag);
    InFlag = Chain.getValue(1);
    
    // Finally, select this to a ret instruction.
    return CurDAG->SelectNodeTo(N, Alpha::RETDAG, MVT::Other, Chain, InFlag);
  }
  case ISD::Constant: {
    uint64_t uval = cast<ConstantSDNode>(N)->getValue();
    int64_t val = (int64_t)uval;
    int32_t val32 = (int32_t)val;
    if (val <= IMM_HIGH + IMM_HIGH * IMM_MULT &&
	val >= IMM_LOW  + IMM_LOW  * IMM_MULT)
      break; //(LDAH (LDA))
    if ((uval >> 32) == 0 && //empty upper bits
	val32 <= IMM_HIGH + IMM_HIGH * IMM_MULT)
      //	val32 >= IMM_LOW  + IMM_LOW  * IMM_MULT) //always true
      break; //(zext (LDAH (LDA)))
    //Else use the constant pool
    MachineConstantPool *CP = BB->getParent()->getConstantPool();
    ConstantUInt *C =
      ConstantUInt::get(Type::getPrimitiveType(Type::ULongTyID) , uval);
    SDOperand Tmp, CPI = CurDAG->getTargetConstantPool(C, MVT::i64);
    Tmp = CurDAG->getTargetNode(Alpha::LDAHr, MVT::i64, CPI, getGlobalBaseReg());
    return CurDAG->SelectNodeTo(N, Alpha::LDQr, MVT::i64, MVT::Other, 
				CPI, Tmp, CurDAG->getEntryNode());
  }
  case ISD::ConstantFP:
    if (ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(N)) {
      bool isDouble = N->getValueType(0) == MVT::f64;
      MVT::ValueType T = isDouble ? MVT::f64 : MVT::f32;
      if (CN->isExactlyValue(+0.0)) {
        return CurDAG->SelectNodeTo(N, isDouble ? Alpha::CPYST : Alpha::CPYSS,
                                    T, CurDAG->getRegister(Alpha::F31, T),
                                    CurDAG->getRegister(Alpha::F31, T));
      } else if ( CN->isExactlyValue(-0.0)) {
        return CurDAG->SelectNodeTo(N, isDouble ? Alpha::CPYSNT : Alpha::CPYSNS,
                                    T, CurDAG->getRegister(Alpha::F31, T),
                                    CurDAG->getRegister(Alpha::F31, T));
      } else {
        abort();
      }
      break;
    }

  case ISD::SETCC:
    if (MVT::isFloatingPoint(N->getOperand(0).Val->getValueType(0))) {
      unsigned Opc = Alpha::WTF;
      ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(2))->get();
      bool rev = false;
      bool isNE = false;
      switch(CC) {
      default: N->dump(); assert(0 && "Unknown FP comparison!");
      case ISD::SETEQ: Opc = Alpha::CMPTEQ; break;
      case ISD::SETLT: Opc = Alpha::CMPTLT; break;
      case ISD::SETLE: Opc = Alpha::CMPTLE; break;
      case ISD::SETGT: Opc = Alpha::CMPTLT; rev = true; break;
      case ISD::SETGE: Opc = Alpha::CMPTLE; rev = true; break;
      case ISD::SETNE: Opc = Alpha::CMPTEQ; isNE = true; break;
      };
      SDOperand tmp1 = Select(N->getOperand(0)),
        tmp2 = Select(N->getOperand(1));
      SDOperand cmp = CurDAG->getTargetNode(Opc, MVT::f64, 
                                            rev?tmp2:tmp1,
                                            rev?tmp1:tmp2);
      if (isNE) 
        cmp = CurDAG->getTargetNode(Alpha::CMPTEQ, MVT::f64, cmp, 
                                    CurDAG->getRegister(Alpha::F31, MVT::f64));
      
      SDOperand LD;
      if (AlphaLowering.hasITOF()) {
        LD = CurDAG->getNode(AlphaISD::FTOIT_, MVT::i64, cmp);
      } else {
        int FrameIdx =
          CurDAG->getMachineFunction().getFrameInfo()->CreateStackObject(8, 8);
        SDOperand FI = CurDAG->getFrameIndex(FrameIdx, MVT::i64);
        SDOperand ST = CurDAG->getTargetNode(Alpha::STT, MVT::Other, 
                                             cmp, FI, CurDAG->getRegister(Alpha::R31, MVT::i64));
        LD = CurDAG->getTargetNode(Alpha::LDQ, MVT::i64, FI, 
                                   CurDAG->getRegister(Alpha::R31, MVT::i64),
                                   ST);
      }
      SDOperand FP = CurDAG->getTargetNode(Alpha::CMPULT, MVT::i64, 
                                           CurDAG->getRegister(Alpha::R31, MVT::i64),
                                           LD);
      return FP;
    }
    break;

  case ISD::SELECT:
    if (MVT::isFloatingPoint(N->getValueType(0)) &&
	(N->getOperand(0).getOpcode() != ISD::SETCC ||
	 !MVT::isFloatingPoint(N->getOperand(0).getOperand(1).getValueType()))) {
      //This should be the condition not covered by the Patterns
      //FIXME: Don't have SelectCode die, but rather return something testable
      // so that things like this can be caught in fall though code
      //move int to fp
      bool isDouble = N->getValueType(0) == MVT::f64;
      SDOperand LD,
	cond = Select(N->getOperand(0)),
	TV = Select(N->getOperand(1)),
	FV = Select(N->getOperand(2));
      
      if (AlphaLowering.hasITOF()) {
	LD = CurDAG->getNode(AlphaISD::ITOFT_, MVT::f64, cond);
      } else {
	int FrameIdx =
	  CurDAG->getMachineFunction().getFrameInfo()->CreateStackObject(8, 8);
	SDOperand FI = CurDAG->getFrameIndex(FrameIdx, MVT::i64);
	SDOperand ST = CurDAG->getTargetNode(Alpha::STQ, MVT::Other,
					     cond, FI, CurDAG->getRegister(Alpha::R31, MVT::i64));
	LD = CurDAG->getTargetNode(Alpha::LDT, MVT::f64, FI,
				   CurDAG->getRegister(Alpha::R31, MVT::i64),
				   ST);
      }
      SDOperand FP = CurDAG->getTargetNode(isDouble?Alpha::FCMOVNET:Alpha::FCMOVNES,
					   MVT::f64, FV, TV, LD);
      return FP;
    }
    break;

  }

  return SelectCode(Op);
}

SDOperand AlphaDAGToDAGISel::SelectCALL(SDOperand Op) {
  //TODO: add flag stuff to prevent nondeturministic breakage!

  SDNode *N = Op.Val;
  SDOperand Chain = Select(N->getOperand(0));
  SDOperand Addr = N->getOperand(1);
  SDOperand InFlag;  // Null incoming flag value.

   std::vector<SDOperand> CallOperands;
   std::vector<MVT::ValueType> TypeOperands;
  
   //grab the arguments
   for(int i = 2, e = N->getNumOperands(); i < e; ++i) {
     TypeOperands.push_back(N->getOperand(i).getValueType());
     CallOperands.push_back(Select(N->getOperand(i)));
   }
   int count = N->getNumOperands() - 2;

   static const unsigned args_int[] = {Alpha::R16, Alpha::R17, Alpha::R18,
                                       Alpha::R19, Alpha::R20, Alpha::R21};
   static const unsigned args_float[] = {Alpha::F16, Alpha::F17, Alpha::F18,
                                         Alpha::F19, Alpha::F20, Alpha::F21};
   
   for (int i = 6; i < count; ++i) {
     unsigned Opc = Alpha::WTF;
     if (MVT::isInteger(TypeOperands[i])) {
       Opc = Alpha::STQ;
     } else if (TypeOperands[i] == MVT::f32) {
       Opc = Alpha::STS;
     } else if (TypeOperands[i] == MVT::f64) {
       Opc = Alpha::STT;
     } else
       assert(0 && "Unknown operand"); 
     Chain = CurDAG->getTargetNode(Opc, MVT::Other, CallOperands[i], 
                                   getI64Imm((i - 6) * 8), 
                                   CurDAG->getCopyFromReg(Chain, Alpha::R30, MVT::i64),
                                   Chain);
   }
   for (int i = 0; i < std::min(6, count); ++i) {
     if (MVT::isInteger(TypeOperands[i])) {
       Chain = CurDAG->getCopyToReg(Chain, args_int[i], CallOperands[i], InFlag);
       InFlag = Chain.getValue(1);
     } else if (TypeOperands[i] == MVT::f32 || TypeOperands[i] == MVT::f64) {
       Chain = CurDAG->getCopyToReg(Chain, args_float[i], CallOperands[i], InFlag);
       InFlag = Chain.getValue(1);
     } else
       assert(0 && "Unknown operand"); 
   }


   // Finally, once everything is in registers to pass to the call, emit the
   // call itself.
   if (Addr.getOpcode() == AlphaISD::GPRelLo) {
     SDOperand GOT = getGlobalBaseReg();
     Chain = CurDAG->getCopyToReg(Chain, Alpha::R29, GOT, InFlag);
     InFlag = Chain.getValue(1);
     Chain = CurDAG->getTargetNode(Alpha::BSR, MVT::Other, MVT::Flag, 
				   Addr.getOperand(0), Chain, InFlag);
   } else {
     Chain = CurDAG->getCopyToReg(Chain, Alpha::R27, Select(Addr), InFlag);
     InFlag = Chain.getValue(1);
     Chain = CurDAG->getTargetNode(Alpha::JSR, MVT::Other, MVT::Flag, 
				   Chain, InFlag );
   }
   InFlag = Chain.getValue(1);

   std::vector<SDOperand> CallResults;
  
   switch (N->getValueType(0)) {
   default: assert(0 && "Unexpected ret value!");
     case MVT::Other: break;
   case MVT::i64:
     Chain = CurDAG->getCopyFromReg(Chain, Alpha::R0, MVT::i64, InFlag).getValue(1);
     CallResults.push_back(Chain.getValue(0));
     break;
   case MVT::f32:
     Chain = CurDAG->getCopyFromReg(Chain, Alpha::F0, MVT::f32, InFlag).getValue(1);
     CallResults.push_back(Chain.getValue(0));
     break;
   case MVT::f64:
     Chain = CurDAG->getCopyFromReg(Chain, Alpha::F0, MVT::f64, InFlag).getValue(1);
     CallResults.push_back(Chain.getValue(0));
     break;
   }

   CallResults.push_back(Chain);
   for (unsigned i = 0, e = CallResults.size(); i != e; ++i)
     CodeGenMap[Op.getValue(i)] = CallResults[i];
   return CallResults[Op.ResNo];
}


/// createAlphaISelDag - This pass converts a legalized DAG into a 
/// Alpha-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createAlphaISelDag(TargetMachine &TM) {
  return new AlphaDAGToDAGISel(TM);
}
