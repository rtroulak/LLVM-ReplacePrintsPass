//===- BPFDisassembler.cpp - Disassembler for BPF ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is part of the BPF Disassembler.
//
//===----------------------------------------------------------------------===//

#include "BPF.h"
#include "BPFRegisterInfo.h"
#include "BPFSubtarget.h"
#include "MCTargetDesc/BPFMCTargetDesc.h"

#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCFixedLenDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "bpf-disassembler"

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {

/// A disassembler class for BPF.
class BPFDisassembler : public MCDisassembler {
public:
  BPFDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}
  virtual ~BPFDisassembler() {}

  DecodeStatus getInstruction(MCInst &Instr, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &VStream,
                              raw_ostream &CStream) const override;
};
}

static MCDisassembler *createBPFDisassembler(const Target &T,
                                             const MCSubtargetInfo &STI,
                                             MCContext &Ctx) {
  return new BPFDisassembler(STI, Ctx);
}


extern "C" void LLVMInitializeBPFDisassembler() {
  // Register the disassembler.
  TargetRegistry::RegisterMCDisassembler(getTheBPFTarget(),
                                         createBPFDisassembler);
  TargetRegistry::RegisterMCDisassembler(getTheBPFleTarget(),
                                         createBPFDisassembler);
  TargetRegistry::RegisterMCDisassembler(getTheBPFbeTarget(),
                                         createBPFDisassembler);
}

static const unsigned GPRDecoderTable[] = {
    BPF::R0,  BPF::R1,  BPF::R2,  BPF::R3,  BPF::R4,  BPF::R5,
    BPF::R6,  BPF::R7,  BPF::R8,  BPF::R9,  BPF::R10, BPF::R11};

static DecodeStatus DecodeGPRRegisterClass(MCInst &Inst, unsigned RegNo,
                                           uint64_t /*Address*/,
                                           const void * /*Decoder*/) {
  if (RegNo > 11)
    return MCDisassembler::Fail;

  unsigned Reg = GPRDecoderTable[RegNo];
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus decodeMemoryOpValue(MCInst &Inst, unsigned Insn,
                                        uint64_t Address, const void *Decoder) {
  unsigned Register = (Insn >> 16) & 0xf;
  Inst.addOperand(MCOperand::createReg(GPRDecoderTable[Register]));
  unsigned Offset = (Insn & 0xffff);
  Inst.addOperand(MCOperand::createImm(SignExtend32<16>(Offset)));

  return MCDisassembler::Success;
}

#include "BPFGenDisassemblerTables.inc"

static DecodeStatus readInstruction64(ArrayRef<uint8_t> Bytes, uint64_t Address,
                                      uint64_t &Size, uint64_t &Insn) {
  uint64_t Lo, Hi;

  if (Bytes.size() < 8) {
    Size = 0;
    return MCDisassembler::Fail;
  }

  Size = 8;
  Hi = (Bytes[0] << 24) | (Bytes[1] << 16) | (Bytes[2] << 0) | (Bytes[3] << 8);
  Lo = (Bytes[4] << 0) | (Bytes[5] << 8) | (Bytes[6] << 16) | (Bytes[7] << 24);
  Insn = Make_64(Hi, Lo);

  return MCDisassembler::Success;
}

DecodeStatus BPFDisassembler::getInstruction(MCInst &Instr, uint64_t &Size,
                                             ArrayRef<uint8_t> Bytes,
                                             uint64_t Address,
                                             raw_ostream &VStream,
                                             raw_ostream &CStream) const {
  uint64_t Insn;
  DecodeStatus Result;

  Result = readInstruction64(Bytes, Address, Size, Insn);
  if (Result == MCDisassembler::Fail) return MCDisassembler::Fail;

  Result = decodeInstruction(DecoderTableBPF64, Instr, Insn,
                             Address, this, STI);
  if (Result == MCDisassembler::Fail) return MCDisassembler::Fail;

  switch (Instr.getOpcode()) {
  case BPF::LD_imm64: {
    if (Bytes.size() < 16) {
      Size = 0;
      return MCDisassembler::Fail;
    }
    Size = 16;
    uint64_t Hi = (Bytes[12] << 0) | (Bytes[13] << 8) | (Bytes[14] << 16) | (Bytes[15] << 24);
    auto& Op = Instr.getOperand(1);
    Op.setImm(Make_64(Hi, Op.getImm()));
    break;
  }
  case BPF::LD_ABS_B:
  case BPF::LD_ABS_H:
  case BPF::LD_ABS_W:
  case BPF::LD_IND_B:
  case BPF::LD_IND_H:
  case BPF::LD_IND_W: {
    auto Op = Instr.getOperand(0);
    Instr.clear();
    Instr.addOperand(MCOperand::createReg(BPF::R6));
    Instr.addOperand(Op);
    break;
  }
  }

  return Result;
}

typedef DecodeStatus (*DecodeFunc)(MCInst &MI, unsigned insn, uint64_t Address,
                                   const void *Decoder);