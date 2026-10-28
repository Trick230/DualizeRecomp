// The Broadway interpreter: every instruction with the semantics DolRecomp's C
// emitter gives it (and through it Dolphin's interpreter), using the same
// GXRuntime helpers for floating point, paired singles, quantized loads and
// stores and exceptions, and the same cycle costs. Real mode is the BAT
// identity mapping through the uncached mirror.

#include "core/cpu.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <bit>

namespace dualize
{
namespace
{
constexpr uint32_t CrShift(uint32_t field)
{
  return 4u * (7u - field);
}

uint32_t Mask32(uint32_t mb, uint32_t me)
{
  const uint32_t begin = 0xFFFFFFFFu >> mb;
  const uint32_t end = me < 31 ? 0xFFFFFFFFu >> (me + 1) : 0;
  return mb <= me ? begin & ~end : begin | ~end;
}

inline uint32_t Rotl(uint32_t value, uint32_t sh)
{
  return std::rotl(value, static_cast<int>(sh & 31));
}

inline double F64(uint64_t bits)
{
  return std::bit_cast<double>(bits);
}
inline uint64_t Bits(double value)
{
  return std::bit_cast<uint64_t>(value);
}
// lfs/stfs bit repacks (Dolphin's ConvertToDouble/ConvertToSingle).
inline double FromSingleBits(uint32_t bits)
{
  return F64(convert_to_double(bits));
}
inline uint32_t ToSingleBits(double value)
{
  return convert_to_single(Bits(value));
}

inline void SetCr0(CPUState& c, uint32_t value)
{
  const int32_t v = static_cast<int32_t>(value);
  const uint32_t bits = (v < 0 ? 8u : v > 0 ? 4u : 2u) | ((c.xer >> 31) & 1u);
  c.cr = (c.cr & 0x0FFFFFFFu) | (bits << 28);
}
inline void SetCr1(CPUState& c)
{
  c.cr = (c.cr & 0xF0FFFFFFu) | ((c.fpscr >> 4) & 0x0F000000u);
}
inline void Compare(CPUState& c, uint32_t field, bool lt, bool gt)
{
  const uint32_t bits = (lt ? 8u : gt ? 4u : 2u) | ((c.xer >> 31) & 1u);
  const uint32_t shift = CrShift(field);
  c.cr = (c.cr & ~(0xFu << shift)) | (bits << shift);
}
inline void SetCarry(CPUState& c, bool carry)
{
  c.xer = (c.xer & ~0x20000000u) | (carry ? 0x20000000u : 0u);
}
inline uint32_t Carry(const CPUState& c)
{
  return (c.xer >> 29) & 1u;
}

// Cycle costs as DolRecomp charges them per block (c_cfg.c instruction_cycles).
enum : int
{
  C_DEFAULT = 1,
};
}  // namespace

bool Cpu::Fetch(uint32_t pc, uint32_t* raw) const
{
  const uint8_t* p = (state.msr & 0x20u) ? m_machine.memory.Effective(pc, 4) :
                                           m_machine.memory.Physical(pc, 4);
  if (!p)
    return false;
  *raw = Load32(p);
  return true;
}

int Cpu::Step()
{
  const uint32_t cia = state.pc;
  uint32_t raw;
  if (!Fetch(cia, &raw))
  {
    // Instruction fetch from nowhere: an ISI, which NiGHTS never takes.
    state.srr0 = cia;
    state.srr1 = (state.msr & 0x87C0FFFFu) | (1u << 30);
    state.msr = (state.msr & ~0x04EF36u) | ((state.msr >> 16) & 1u);
    state.pc = 0x400;
    return 1;
  }
  ++m_stats.interpreted;
  return Execute(raw, cia);
}

int Cpu::Execute(uint32_t raw, uint32_t cia)
{
  CPUState& c = state;
  const uint32_t op = raw >> 26;
  const uint32_t rd = (raw >> 21) & 31;
  const uint32_t ra = (raw >> 16) & 31;
  const uint32_t rb = (raw >> 11) & 31;
  const uint32_t rc_reg = (raw >> 6) & 31;
  const bool rc = raw & 1;
  const int32_t simm = static_cast<int16_t>(raw & 0xFFFF);
  const uint32_t uimm = raw & 0xFFFF;
  const uint32_t x10 = (raw >> 1) & 0x3FF;
  uint32_t next = cia + 4;
  int cycles = C_DEFAULT;

  // Effective addresses: D-form and X-form, with rA = 0 meaning 0 unless the
  // form updates rA.
  const auto ea_d = [&]() { return (ra ? c.gpr[ra] : 0u) + static_cast<uint32_t>(simm); };
  const auto ea_x = [&]() { return (ra ? c.gpr[ra] : 0u) + c.gpr[rb]; };
  const auto da = [&](uint32_t ea) { return DataAddress(ea); };
  const auto fp_ok = [&]() { return ppc_fp_available_inline(&c, cia); };
  const auto illegal = [&]() { ppc_program_exception(&c, PPC_PROGRAM_ILLEGAL, cia); };

  switch (op)
  {
  case 3:  // twi
    cycles = 1;
    if (ppc_trap_condition(static_cast<u8>(rd), c.gpr[ra], static_cast<uint32_t>(simm)))
      ppc_program_exception(&c, PPC_PROGRAM_TRAP, cia);
    break;

  case 4:  // paired singles
  {
    if (x10 == 1014)  // dcbz_l
    {
      ppc_dcbz_l(&c, da(ea_x()), cia);
      break;
    }
    if (!fp_ok())
      break;
    const uint32_t x6 = x10 & 0x3F;
    if (x6 == 6 || x6 == 7 || x6 == 38 || x6 == 39)
    {
      // psq_lx, psq_stx, psq_lux, psq_stux
      const bool update = x6 >= 38;
      const uint32_t ea = update ? c.gpr[ra] + c.gpr[rb] : ea_x();
      const bool w = (raw >> 10) & 1;
      const uint8_t i = static_cast<uint8_t>((raw >> 7) & 7);
      const bool ok = (x6 & 1) ? ppc_psq_store_inline(&c, static_cast<u8>(rd), da(ea), w, i, true, cia) :
                                 ppc_psq_load_inline(&c, static_cast<u8>(rd), da(ea), w, i, true, cia);
      if (ok && !c.exception && update)
        c.gpr[ra] = ea;
      break;
    }
    switch (x10)
    {
    case 0:    // ps_cmpu0
    case 32:   // ps_cmpo0
    case 64:   // ps_cmpu1
    case 96:   // ps_cmpo1
    {
      const bool lane1 = x10 == 64 || x10 == 96;
      ppc_fcmp_inline(&c, static_cast<u8>(rd >> 2), lane1 ? c.ps1[ra] : c.fpr[ra],
                      lane1 ? c.ps1[rb] : c.fpr[rb], x10 == 32 || x10 == 96);
      goto done;
    }
    case 40:  // ps_neg
      c.fpr[rd] = FromSingleBits(ToSingleBits(c.fpr[rb]) ^ 0x80000000u);
      c.ps1[rd] = FromSingleBits(ToSingleBits(c.ps1[rb]) ^ 0x80000000u);
      break;
    case 72:  // ps_mr
      c.fpr[rd] = c.fpr[rb];
      c.ps1[rd] = c.ps1[rb];
      break;
    case 136:  // ps_nabs
      c.fpr[rd] = FromSingleBits(ToSingleBits(c.fpr[rb]) | 0x80000000u);
      c.ps1[rd] = FromSingleBits(ToSingleBits(c.ps1[rb]) | 0x80000000u);
      break;
    case 264:  // ps_abs
      c.fpr[rd] = FromSingleBits(ToSingleBits(c.fpr[rb]) & 0x7FFFFFFFu);
      c.ps1[rd] = FromSingleBits(ToSingleBits(c.ps1[rb]) & 0x7FFFFFFFu);
      break;
    case 528:  // ps_merge00
    case 560:  // ps_merge01
    case 592:  // ps_merge10
    case 624:  // ps_merge11
    {
      const double ps0 = (x10 == 592 || x10 == 624) ? c.ps1[ra] : c.fpr[ra];
      const double ps1 = (x10 == 560 || x10 == 624) ? c.ps1[rb] : c.fpr[rb];
      c.fpr[rd] = ps0;
      c.ps1[rd] = ps1;
      break;
    }
    default:
      switch (x10 & 0x1F)
      {
      case 10:
        ppc_ps_sum_inline(&c, rd, ra, rc_reg, rb, false);
        break;
      case 11:
        ppc_ps_sum_inline(&c, rd, ra, rc_reg, rb, true);
        break;
      case 12:
        ppc_ps_muls_inline(&c, rd, ra, rc_reg, false);
        break;
      case 13:
        ppc_ps_muls_inline(&c, rd, ra, rc_reg, true);
        break;
      case 14:
        ppc_ps_madds_inline(&c, rd, ra, rc_reg, rb, false);
        break;
      case 15:
        ppc_ps_madds_inline(&c, rd, ra, rc_reg, rb, true);
        break;
      case 18:
        ppc_ps_div_op(&c, rd, ra, rb);
        cycles = 17;
        break;
      case 20:
        ppc_ps_sub_inline(&c, rd, ra, rb);
        break;
      case 21:
        ppc_ps_add_op(&c, rd, ra, rb);
        break;
      case 23:  // ps_sel
        c.fpr[rd] = static_cast<float>(c.fpr[ra]) >= 0.0f ? c.fpr[rc_reg] : c.fpr[rb];
        c.ps1[rd] = static_cast<float>(c.ps1[ra]) >= 0.0f ? c.ps1[rc_reg] : c.ps1[rb];
        break;
      case 24:
        ppc_ps_res_op(&c, rd, rb);
        break;
      case 25:
        ppc_ps_mul_inline(&c, rd, ra, rc_reg);
        break;
      case 26:
        ppc_ps_rsqrte_op(&c, rd, rb);
        cycles = 2;
        break;
      case 28:
        ppc_ps_madd_inline(&c, rd, ra, rc_reg, rb, true, false);
        break;
      case 29:
        ppc_ps_madd_inline(&c, rd, ra, rc_reg, rb, false, false);
        break;
      case 30:
        ppc_ps_madd_inline(&c, rd, ra, rc_reg, rb, true, true);
        break;
      case 31:
        ppc_ps_madd_inline(&c, rd, ra, rc_reg, rb, false, true);
        break;
      default:
        illegal();
        goto done;
      }
      break;
    }
    if (rc && !c.exception)
      SetCr1(c);
    break;
  }

  case 7:  // mulli
    c.gpr[rd] = static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(c.gpr[ra])) * simm);
    cycles = 3;
    break;
  case 8:  // subfic
  {
    const uint64_t res = static_cast<uint64_t>(static_cast<uint32_t>(simm)) + (~c.gpr[ra]) + 1u;
    c.gpr[rd] = static_cast<uint32_t>(res);
    SetCarry(c, (res >> 32) & 1);
    break;
  }
  case 10:  // cmpli
    Compare(c, rd >> 2, c.gpr[ra] < uimm, c.gpr[ra] > uimm);
    break;
  case 11:  // cmpi
    Compare(c, rd >> 2, static_cast<int32_t>(c.gpr[ra]) < simm, static_cast<int32_t>(c.gpr[ra]) > simm);
    break;
  case 12:  // addic
  case 13:  // addic.
  {
    const uint64_t res = static_cast<uint64_t>(c.gpr[ra]) + static_cast<uint32_t>(simm);
    c.gpr[rd] = static_cast<uint32_t>(res);
    SetCarry(c, (res >> 32) & 1);
    if (op == 13)
      SetCr0(c, c.gpr[rd]);
    break;
  }
  case 14:  // addi
    c.gpr[rd] = (ra ? c.gpr[ra] : 0u) + static_cast<uint32_t>(simm);
    break;
  case 15:  // addis
    c.gpr[rd] = (ra ? c.gpr[ra] : 0u) + (static_cast<uint32_t>(simm) << 16);
    break;

  case 16:  // bc
  {
    const uint32_t bo = rd, bi = ra;
    if (!(bo & 4))
      --c.ctr;
    const bool ctr_ok = (bo & 4) || ((c.ctr != 0) ^ ((bo >> 1) & 1));
    const bool cond_ok = (bo & 16) || (((c.cr >> (31 - bi)) & 1) == ((bo >> 3) & 1));
    if (raw & 1)
      c.lr = cia + 4;
    if (ctr_ok && cond_ok)
    {
      const uint32_t disp = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw & 0xFFFC)));
      next = (raw & 2) ? disp : cia + disp;
    }
    break;
  }
  case 17:  // sc
    ppc_system_call_exception(&c, cia);
    cycles = 2;
    break;
  case 18:  // b
  {
    uint32_t disp = raw & 0x03FFFFFC;
    if (disp & 0x02000000)
      disp |= 0xFC000000;
    if (raw & 1)
      c.lr = cia + 4;
    next = (raw & 2) ? disp : cia + disp;
    break;
  }

  case 19:
    switch (x10)
    {
    case 0:  // mcrf
    {
      const uint32_t bits = (c.cr >> CrShift(ra >> 2)) & 0xF;
      c.cr = (c.cr & ~(0xFu << CrShift(rd >> 2))) | (bits << CrShift(rd >> 2));
      break;
    }
    case 16:   // bclr
    case 528:  // bcctr
    {
      const uint32_t bo = rd, bi = ra;
      const uint32_t target = (x10 == 16 ? c.lr : c.ctr) & ~3u;
      bool ctr_ok = true;
      if (x10 == 16 && !(bo & 4))
      {
        --c.ctr;
        ctr_ok = (c.ctr != 0) ^ ((bo >> 1) & 1);
      }
      const bool cond_ok = (bo & 16) || (((c.cr >> (31 - bi)) & 1) == ((bo >> 3) & 1));
      if (raw & 1)
        c.lr = cia + 4;
      if (ctr_ok && cond_ok)
        next = target;
      break;
    }
    case 50:  // rfi
      ppc_rfi(&c, cia);
      cycles = 2;
      if (!c.exception)
        next = c.pc;
      break;
    case 150:  // isync
      break;
    case 33:   // crnor
    case 129:  // crandc
    case 193:  // crxor
    case 225:  // crnand
    case 257:  // crand
    case 289:  // creqv
    case 417:  // crorc
    case 449:  // cror
    {
      const uint32_t a = (c.cr >> (31 - ra)) & 1, b = (c.cr >> (31 - rb)) & 1;
      uint32_t v;
      switch (x10)
      {
      case 33: v = ~(a | b); break;
      case 129: v = a & ~b; break;
      case 193: v = a ^ b; break;
      case 225: v = ~(a & b); break;
      case 257: v = a & b; break;
      case 289: v = ~(a ^ b); break;
      case 417: v = a | ~b; break;
      default: v = a | b; break;
      }
      const uint32_t mask = 0x80000000u >> rd;
      c.cr = (c.cr & ~mask) | ((v & 1) ? mask : 0u);
      break;
    }
    default:
      illegal();
      break;
    }
    break;

  case 20:  // rlwimi
  {
    const uint32_t mask = Mask32((raw >> 6) & 31, (raw >> 1) & 31);
    c.gpr[ra] = (c.gpr[ra] & ~mask) | (Rotl(c.gpr[rd], rb) & mask);
    if (rc)
      SetCr0(c, c.gpr[ra]);
    break;
  }
  case 21:  // rlwinm
    c.gpr[ra] = Rotl(c.gpr[rd], rb) & Mask32((raw >> 6) & 31, (raw >> 1) & 31);
    if (rc)
      SetCr0(c, c.gpr[ra]);
    break;
  case 23:  // rlwnm
    c.gpr[ra] = Rotl(c.gpr[rd], c.gpr[rb]) & Mask32((raw >> 6) & 31, (raw >> 1) & 31);
    if (rc)
      SetCr0(c, c.gpr[ra]);
    break;
  case 24:  // ori
    c.gpr[ra] = c.gpr[rd] | uimm;
    break;
  case 25:  // oris
    c.gpr[ra] = c.gpr[rd] | (uimm << 16);
    break;
  case 26:  // xori
    c.gpr[ra] = c.gpr[rd] ^ uimm;
    break;
  case 27:  // xoris
    c.gpr[ra] = c.gpr[rd] ^ (uimm << 16);
    break;
  case 28:  // andi.
    c.gpr[ra] = c.gpr[rd] & uimm;
    SetCr0(c, c.gpr[ra]);
    break;
  case 29:  // andis.
    c.gpr[ra] = c.gpr[rd] & (uimm << 16);
    SetCr0(c, c.gpr[ra]);
    break;

  case 31:
  {
    const uint32_t rs = rd;
    switch (x10)
    {
    case 0:  // cmp
      Compare(c, rd >> 2, static_cast<int32_t>(c.gpr[ra]) < static_cast<int32_t>(c.gpr[rb]),
              static_cast<int32_t>(c.gpr[ra]) > static_cast<int32_t>(c.gpr[rb]));
      goto done;
    case 32:  // cmpl
      Compare(c, rd >> 2, c.gpr[ra] < c.gpr[rb], c.gpr[ra] > c.gpr[rb]);
      goto done;
    case 4:  // tw
      cycles = 2;
      if (ppc_trap_condition(static_cast<u8>(rd), c.gpr[ra], c.gpr[rb]))
        ppc_program_exception(&c, PPC_PROGRAM_TRAP, cia);
      break;
    case 19:  // mfcr
      c.gpr[rd] = c.cr;
      break;
    case 144:  // mtcrf
    {
      const uint32_t crm = (raw >> 12) & 0xFF;
      uint32_t mask = 0;
      for (uint32_t field = 0; field < 8; ++field)
        if (crm & (0x80u >> field))
          mask |= 0xFu << CrShift(field);
      c.cr = (c.cr & ~mask) | (c.gpr[rs] & mask);
      break;
    }
    case 512:  // mcrxr
    {
      const uint32_t shift = CrShift(rd >> 2);
      c.cr = (c.cr & ~(0xFu << shift)) | (((c.xer >> 28) & 0xF) << shift);
      c.xer &= ~0xE0000000u;
      break;
    }
    case 83:  // mfmsr
      c.gpr[rd] = c.msr;
      break;
    case 146:  // mtmsr
      c.msr = c.gpr[rs];
      RequestInterruptCheck();
      break;
    case 595:  // mfsr
      c.gpr[rd] = c.sr[ra & 15];
      cycles = 3;
      break;
    case 659:  // mfsrin
      c.gpr[rd] = c.sr[c.gpr[rb] >> 28];
      cycles = 3;
      break;
    case 210:  // mtsr
      c.sr[ra & 15] = c.gpr[rs];
      break;
    case 242:  // mtsrin
      c.sr[c.gpr[rb] >> 28] = c.gpr[rs];
      break;
    case 339:  // mfspr
    case 371:  // mftb
    {
      const uint16_t spr = static_cast<uint16_t>(((raw >> 16) & 31) | ((raw >> 6) & 0x3E0));
      const uint32_t value =
          x10 == 371 ? ppc_mftb(&c, spr, cia) : ppc_mfspr(&c, spr, cia);
      if (!c.exception)
        c.gpr[rd] = value;
      break;
    }
    case 467:  // mtspr
    {
      const uint16_t spr = static_cast<uint16_t>(((raw >> 16) & 31) | ((raw >> 6) & 0x3E0));
      ppc_mtspr(&c, spr, c.gpr[rs], cia);
      cycles = 2;
      break;
    }

    case 28:  // and
      c.gpr[ra] = c.gpr[rs] & c.gpr[rb];
      goto logical_record;
    case 60:  // andc
      c.gpr[ra] = c.gpr[rs] & ~c.gpr[rb];
      goto logical_record;
    case 444:  // or
      c.gpr[ra] = c.gpr[rs] | c.gpr[rb];
      goto logical_record;
    case 412:  // orc
      c.gpr[ra] = c.gpr[rs] | ~c.gpr[rb];
      goto logical_record;
    case 316:  // xor
      c.gpr[ra] = c.gpr[rs] ^ c.gpr[rb];
      goto logical_record;
    case 476:  // nand
      c.gpr[ra] = ~(c.gpr[rs] & c.gpr[rb]);
      goto logical_record;
    case 124:  // nor
      c.gpr[ra] = ~(c.gpr[rs] | c.gpr[rb]);
      goto logical_record;
    case 284:  // eqv
      c.gpr[ra] = ~(c.gpr[rs] ^ c.gpr[rb]);
      goto logical_record;
    case 26:  // cntlzw
      c.gpr[ra] = static_cast<uint32_t>(std::countl_zero(c.gpr[rs]));
      goto logical_record;
    case 954:  // extsb
      c.gpr[ra] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(c.gpr[rs])));
      goto logical_record;
    case 922:  // extsh
      c.gpr[ra] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(c.gpr[rs])));
      goto logical_record;
    case 24:  // slw
    {
      const uint32_t sh = c.gpr[rb] & 0x3F;
      c.gpr[ra] = sh > 31 ? 0u : c.gpr[rs] << sh;
      goto logical_record;
    }
    case 536:  // srw
    {
      const uint32_t sh = c.gpr[rb] & 0x3F;
      c.gpr[ra] = sh > 31 ? 0u : c.gpr[rs] >> sh;
      goto logical_record;
    }
    case 792:  // sraw
    case 824:  // srawi
    {
      const uint32_t sh = x10 == 824 ? rb : c.gpr[rb] & 0x3F;
      const uint32_t value = c.gpr[rs];
      bool ca = false;
      if (sh == 0)
        c.gpr[ra] = value;
      else if (sh > 31)
      {
        c.gpr[ra] = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        ca = (value & 0x80000000u) != 0;
      }
      else
      {
        c.gpr[ra] = static_cast<uint32_t>(static_cast<int32_t>(value) >> sh);
        ca = (value & 0x80000000u) && (value << (32 - sh)) != 0;
      }
      SetCarry(c, ca);
      goto logical_record;
    }
    logical_record:
      if (rc)
        SetCr0(c, c.gpr[ra]);
      break;

    // Loads and stores, X-form.
    case 23:   // lwzx
    case 55:   // lwzux
    case 87:   // lbzx
    case 119:  // lbzux
    case 279:  // lhzx
    case 311:  // lhzux
    case 343:  // lhax
    case 375:  // lhaux
    case 534:  // lwbrx
    case 790:  // lhbrx
    {
      const bool update = x10 == 55 || x10 == 119 || x10 == 311 || x10 == 375;
      const uint32_t ea = update ? c.gpr[ra] + c.gpr[rb] : ea_x();
      const uint32_t a = da(ea);
      uint32_t value;
      switch (x10)
      {
      case 23: case 55: value = mem_read32_at(&c, cia, a); break;
      case 87: case 119: value = mem_read8_at(&c, cia, a); break;
      case 279: case 311: value = mem_read16_at(&c, cia, a); break;
      case 343: case 375:
        value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(mem_read16_at(&c, cia, a))));
        break;
      case 534: value = __builtin_bswap32(mem_read32_at(&c, cia, a)); break;
      default: value = __builtin_bswap16(mem_read16_at(&c, cia, a)); break;
      }
      c.gpr[rd] = value;
      if (update)
        c.gpr[ra] = ea;
      break;
    }
    case 151:  // stwx
    case 183:  // stwux
    case 215:  // stbx
    case 247:  // stbux
    case 407:  // sthx
    case 439:  // sthux
    case 662:  // stwbrx
    case 918:  // sthbrx
    {
      const bool update = x10 == 183 || x10 == 247 || x10 == 439;
      const uint32_t ea = update ? c.gpr[ra] + c.gpr[rb] : ea_x();
      const uint32_t a = da(ea);
      switch (x10)
      {
      case 151: case 183: mem_write32_at(&c, cia, a, c.gpr[rs]); break;
      case 215: case 247: mem_write8_at(&c, cia, a, static_cast<u8>(c.gpr[rs])); break;
      case 407: case 439: mem_write16_at(&c, cia, a, static_cast<u16>(c.gpr[rs])); break;
      case 662: mem_write32_at(&c, cia, a, __builtin_bswap32(c.gpr[rs])); break;
      default: mem_write16_at(&c, cia, a, __builtin_bswap16(static_cast<u16>(c.gpr[rs]))); break;
      }
      if (update)
        c.gpr[ra] = ea;
      break;
    }
    case 535:  // lfsx
    case 567:  // lfsux
    case 599:  // lfdx
    case 631:  // lfdux
    case 663:  // stfsx
    case 695:  // stfsux
    case 727:  // stfdx
    case 759:  // stfdux
    case 983:  // stfiwx
    {
      if (!fp_ok())
        break;
      const bool update = x10 == 567 || x10 == 631 || x10 == 695 || x10 == 759;
      const uint32_t ea = update ? c.gpr[ra] + c.gpr[rb] : ea_x();
      const uint32_t a = da(ea);
      switch (x10)
      {
      case 535: case 567:
        c.fpr[rd] = c.ps1[rd] = FromSingleBits(mem_read32_at(&c, cia, a));
        break;
      case 599: case 631: c.fpr[rd] = F64(mem_read64_at(&c, cia, a)); break;
      case 663: case 695: mem_write32_at(&c, cia, a, ToSingleBits(c.fpr[rs])); break;
      case 727: case 759: mem_write64_at(&c, cia, a, Bits(c.fpr[rs])); break;
      default: mem_write32_at(&c, cia, a, static_cast<uint32_t>(Bits(c.fpr[rs]))); break;
      }
      if (update)
        c.gpr[ra] = ea;
      break;
    }
    case 20:  // lwarx
    {
      const uint32_t ea = ea_x();
      c.gpr[rd] = mem_read32_at(&c, cia, da(ea));
      c.reserve_addr = ea;
      c.reserve_valid = true;
      break;
    }
    case 150:  // stwcx.
    {
      const uint32_t ea = ea_x();
      const bool success = c.reserve_valid && ea == c.reserve_addr;
      if (success)
      {
        mem_write32_at(&c, cia, da(ea), c.gpr[rs]);
        c.reserve_valid = false;
      }
      c.cr = (c.cr & 0x0FFFFFFFu) | ((success ? 2u : 0u) << 28) | ((c.xer >> 3) & 0x10000000u);
      break;
    }
    case 533:  // lswx
    case 597:  // lswi
    {
      uint32_t ea, count;
      if (x10 == 533)
      {
        ea = ea_x();
        count = c.xer & 0x7F;
        for (uint32_t r = 0; r < (count + 3) / 4; ++r)
        {
          const uint32_t reg = (rd + r) & 31;
          if (reg == ra || reg == rb)
          {
            illegal();
            goto done;
          }
        }
      }
      else
      {
        ea = ra ? c.gpr[ra] : 0u;
        count = rb ? rb : 32;
      }
      for (uint32_t n = 0; n < count; ++n)
      {
        const uint32_t reg = (rd + n / 4) & 31;
        if ((n & 3) == 0)
          c.gpr[reg] = 0;
        c.gpr[reg] |= static_cast<uint32_t>(mem_read8_at(&c, cia, da(ea + n))) << (24 - 8 * (n & 3));
      }
      break;
    }
    case 661:  // stswx
    case 725:  // stswi
    {
      uint32_t ea, count;
      if (x10 == 661)
      {
        ea = ea_x();
        count = c.xer & 0x7F;
      }
      else
      {
        ea = ra ? c.gpr[ra] : 0u;
        count = rb ? rb : 32;
      }
      for (uint32_t n = 0; n < count; ++n)
      {
        const uint32_t reg = (rs + n / 4) & 31;
        mem_write8_at(&c, cia, da(ea + n), static_cast<u8>(c.gpr[reg] >> (24 - 8 * (n & 3))));
      }
      break;
    }

    // Cache and synchronization.
    case 1014:  // dcbz
    {
      const uint32_t a = da(ea_x() & ~31u);
      for (uint32_t i = 0; i < 32; i += 4)
        mem_write32_at(&c, cia, a + i, 0);
      cycles = 5;
      break;
    }
    case 54:   // dcbst
    case 86:   // dcbf
    case 470:  // dcbi
      ppc_cache_control(&c, x10 == 54 ? PPC_CACHE_DCBST : x10 == 86 ? PPC_CACHE_DCBF : PPC_CACHE_DCBI,
                        ea_x(), cia);
      cycles = 5;
      break;
    case 982:  // icbi
      ppc_cache_control(&c, PPC_CACHE_ICBI, ea_x(), cia);
      cycles = 4;
      break;
    case 246:  // dcbtst
    case 278:  // dcbt
      cycles = 2;
      break;
    case 598:  // sync
      cycles = 3;
      break;
    case 854:  // eieio
    case 566:  // tlbsync
      break;
    case 306:  // tlbie
      ppc_tlbie(&c, c.gpr[rb], cia);
      break;
    case 310:  // eciwx
    {
      const uint32_t value = ppc_eciwx(&c, ea_x(), cia);
      if (!c.exception)
        c.gpr[rd] = value;
      break;
    }
    case 438:  // ecowx
      ppc_ecowx(&c, ea_x(), c.gpr[rs], cia);
      break;

    default:
    {
      // XO-form arithmetic, with or without OE.
      const bool oe = x10 & 0x200;
      const uint32_t a = c.gpr[ra], b = c.gpr[rb];
      uint32_t res;
      switch (x10 & 0x1FF)
      {
      case 266:  // add
        res = a + b;
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(a, b, res));
        break;
      case 10:  // addc
      {
        const uint64_t wide = static_cast<uint64_t>(a) + b;
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(a, b, res));
        break;
      }
      case 138:  // adde
      {
        const uint64_t wide = static_cast<uint64_t>(a) + b + Carry(c);
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(a, b, res));
        break;
      }
      case 234:  // addme
      {
        const uint64_t wide = static_cast<uint64_t>(a) + 0xFFFFFFFFull + Carry(c);
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(a, 0xFFFFFFFFu, res));
        break;
      }
      case 202:  // addze
      {
        const uint64_t wide = static_cast<uint64_t>(a) + Carry(c);
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(a, 0u, res));
        break;
      }
      case 40:  // subf
        res = ~a + b + 1u;
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(~a, b, res));
        break;
      case 8:  // subfc
      {
        const uint64_t wide = static_cast<uint64_t>(b) + (~a) + 1u;
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(~a, b, res));
        break;
      }
      case 136:  // subfe
      {
        const uint64_t wide = static_cast<uint64_t>(~a) + b + Carry(c);
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(~a, b, res));
        break;
      }
      case 232:  // subfme
      {
        const uint64_t wide = static_cast<uint64_t>(~a) + 0xFFFFFFFFull + Carry(c);
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(~a, 0xFFFFFFFFu, res));
        break;
      }
      case 200:  // subfze
      {
        const uint64_t wide = static_cast<uint64_t>(~a) + Carry(c);
        res = static_cast<uint32_t>(wide);
        SetCarry(c, wide >> 32);
        if (oe)
          ppc_set_xer_ov(&c, ppc_add_overflowed(~a, 0u, res));
        break;
      }
      case 104:  // neg
        res = ~a + 1u;
        if (oe)
          ppc_set_xer_ov(&c, a == 0x80000000u);
        break;
      case 235:  // mullw
      {
        const int64_t product = static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b);
        res = static_cast<uint32_t>(product);
        if (oe)
          ppc_set_xer_ov(&c, product < -0x80000000ll || product > 0x7fffffffll);
        cycles = 5;
        break;
      }
      case 75:  // mulhw
        res = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b)) >> 32);
        cycles = 5;
        break;
      case 11:  // mulhwu
        res = static_cast<uint32_t>((static_cast<uint64_t>(a) * b) >> 32);
        cycles = 5;
        break;
      case 491:  // divw
      {
        const int32_t dividend = static_cast<int32_t>(a), divisor = static_cast<int32_t>(b);
        const bool ov = divisor == 0 || (a == 0x80000000u && divisor == -1);
        res = ov ? (dividend < 0 ? 0xFFFFFFFFu : 0u) : static_cast<uint32_t>(dividend / divisor);
        if (oe)
          ppc_set_xer_ov(&c, ov);
        cycles = 40;
        break;
      }
      case 459:  // divwu
        res = b == 0 ? 0u : a / b;
        if (oe)
          ppc_set_xer_ov(&c, b == 0);
        cycles = 40;
        break;
      default:
        illegal();
        goto done;
      }
      c.gpr[rd] = res;
      if (rc)
        SetCr0(c, res);
      break;
    }
    }
    break;
  }

  // Loads and stores, D-form.
  case 32:  // lwz
  case 33:  // lwzu
  case 34:  // lbz
  case 35:  // lbzu
  case 40:  // lhz
  case 41:  // lhzu
  case 42:  // lha
  case 43:  // lhau
  {
    const bool update = op & 1;
    const uint32_t ea = update ? c.gpr[ra] + static_cast<uint32_t>(simm) : ea_d();
    const uint32_t a = da(ea);
    uint32_t value;
    switch (op & ~1u)
    {
    case 32: value = mem_read32_at(&c, cia, a); break;
    case 34: value = mem_read8_at(&c, cia, a); break;
    case 40: value = mem_read16_at(&c, cia, a); break;
    default:
      value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(mem_read16_at(&c, cia, a))));
      break;
    }
    c.gpr[rd] = value;
    if (update)
      c.gpr[ra] = ea;
    break;
  }
  case 36:  // stw
  case 37:  // stwu
  case 38:  // stb
  case 39:  // stbu
  case 44:  // sth
  case 45:  // sthu
  {
    const bool update = op & 1;
    const uint32_t ea = update ? c.gpr[ra] + static_cast<uint32_t>(simm) : ea_d();
    const uint32_t a = da(ea);
    switch (op & ~1u)
    {
    case 36: mem_write32_at(&c, cia, a, c.gpr[rd]); break;
    case 38: mem_write8_at(&c, cia, a, static_cast<u8>(c.gpr[rd])); break;
    default: mem_write16_at(&c, cia, a, static_cast<u16>(c.gpr[rd])); break;
    }
    if (update)
      c.gpr[ra] = ea;
    break;
  }
  case 46:  // lmw
  {
    uint32_t ea = ea_d();
    for (uint32_t r = rd; r < 32; ++r, ea += 4)
      c.gpr[r] = mem_read32_at(&c, cia, da(ea));
    cycles = 11;
    break;
  }
  case 47:  // stmw
  {
    uint32_t ea = ea_d();
    for (uint32_t r = rd; r < 32; ++r, ea += 4)
      mem_write32_at(&c, cia, da(ea), c.gpr[r]);
    cycles = 11;
    break;
  }
  case 48:  // lfs
  case 49:  // lfsu
  case 50:  // lfd
  case 51:  // lfdu
  case 52:  // stfs
  case 53:  // stfsu
  case 54:  // stfd
  case 55:  // stfdu
  {
    if (!fp_ok())
      break;
    const bool update = op & 1;
    const uint32_t ea = update ? c.gpr[ra] + static_cast<uint32_t>(simm) : ea_d();
    const uint32_t a = da(ea);
    switch (op & ~1u)
    {
    case 48: c.fpr[rd] = c.ps1[rd] = FromSingleBits(mem_read32_at(&c, cia, a)); break;
    case 50: c.fpr[rd] = F64(mem_read64_at(&c, cia, a)); break;
    case 52: mem_write32_at(&c, cia, a, ToSingleBits(c.fpr[rd])); break;
    default: mem_write64_at(&c, cia, a, Bits(c.fpr[rd])); break;
    }
    if (update)
      c.gpr[ra] = ea;
    break;
  }
  case 56:  // psq_l
  case 57:  // psq_lu
  case 60:  // psq_st
  case 61:  // psq_stu
  {
    if (!fp_ok())
      break;
    const bool update = op & 1;
    const int32_t offset = static_cast<int32_t>(static_cast<uint32_t>(raw & 0xFFF) << 20) >> 20;
    const uint32_t ea = (update || ra ? c.gpr[ra] : 0u) + static_cast<uint32_t>(offset);
    const bool w = (raw >> 15) & 1;
    const uint8_t i = static_cast<uint8_t>((raw >> 12) & 7);
    const bool ok = op >= 60 ? ppc_psq_store_inline(&c, static_cast<u8>(rd), da(ea), w, i, false, cia) :
                               ppc_psq_load_inline(&c, static_cast<u8>(rd), da(ea), w, i, false, cia);
    if (ok && !c.exception && update)
      c.gpr[ra] = ea;
    break;
  }

  case 59:  // single-precision arithmetic
  {
    if (!fp_ok())
      break;
    switch ((raw >> 1) & 0x1F)
    {
    case 18:
      ppc_fdivs_inline(&c, rd, ra, rb);
      cycles = 17;
      break;
    case 20:
      ppc_fsubs_inline(&c, rd, ra, rb);
      break;
    case 21:
      ppc_fadds_inline(&c, rd, ra, rb);
      break;
    case 24:  // fres
    {
      double result;
      if (ppc_fres(&c, c.fpr[rb], &result))
        c.fpr[rd] = c.ps1[rd] = result;
      break;
    }
    case 25:
      ppc_fmuls_inline(&c, rd, ra, rc_reg);
      break;
    case 28:  // fmsubs
    case 29:  // fmadds
    case 30:  // fnmsubs
    case 31:  // fnmadds
    {
      const uint32_t x5 = (raw >> 1) & 0x1F;
      double result;
      if (ppc_fma_inline(&c, c.fpr[ra], c.fpr[rc_reg], c.fpr[rb], true, x5 == 28 || x5 == 30,
                         x5 >= 30, &result))
        c.fpr[rd] = c.ps1[rd] = result;
      break;
    }
    default:
      illegal();
      goto done;
    }
    if (rc && !c.exception)
      SetCr1(c);
    break;
  }

  case 63:  // double-precision arithmetic and FPSCR
  {
    if (!fp_ok())
      break;
    const uint32_t x5 = (raw >> 1) & 0x1F;
    if (x5 >= 16)
    {
      switch (x5)
      {
      case 18:
        ppc_fdiv_inline(&c, rd, ra, rb);
        cycles = 31;
        break;
      case 20:
        ppc_fsub_inline(&c, rd, ra, rb);
        break;
      case 21:
        ppc_fadd_inline(&c, rd, ra, rb);
        break;
      case 23:  // fsel
        c.fpr[rd] = c.fpr[ra] >= 0.0 ? c.fpr[rc_reg] : c.fpr[rb];
        break;
      case 25:
        ppc_fmul_inline(&c, rd, ra, rc_reg);
        break;
      case 26:  // frsqrte
      {
        double result;
        if (ppc_frsqrte(&c, c.fpr[rb], &result))
          c.fpr[rd] = result;
        break;
      }
      case 28:  // fmsub
      case 29:  // fmadd
      case 30:  // fnmsub
      case 31:  // fnmadd
      {
        double result;
        if (ppc_fma_inline(&c, c.fpr[ra], c.fpr[rc_reg], c.fpr[rb], false, x5 == 28 || x5 == 30,
                           x5 >= 30, &result))
          c.fpr[rd] = result;
        break;
      }
      default:
        illegal();
        goto done;
      }
      if (rc && !c.exception)
        SetCr1(c);
      break;
    }
    switch (x10)
    {
    case 0:   // fcmpu
    case 32:  // fcmpo
      ppc_fcmp_inline(&c, static_cast<u8>(rd >> 2), c.fpr[ra], c.fpr[rb], x10 == 32);
      goto done;
    case 12:  // frsp
      ppc_frsp_inline(&c, rd, rb);
      break;
    case 14:  // fctiw
    case 15:  // fctiwz
    {
      uint64_t result;
      if (ppc_fctiw(&c, c.fpr[rb], x10 == 15, &result))
        c.fpr[rd] = F64(result);
      break;
    }
    case 38:  // mtfsb1
    case 70:  // mtfsb0
    {
      const uint32_t mask = 0x80000000u >> rd;
      if (rd != 1 && rd != 2)
        c.fpscr = x10 == 38 ? c.fpscr | mask : c.fpscr & ~mask;
      cycles = 3;
      break;
    }
    case 40:  // fneg
      c.fpr[rd] = F64(Bits(c.fpr[rb]) ^ 0x8000000000000000ull);
      break;
    case 72:  // fmr
      c.fpr[rd] = c.fpr[rb];
      break;
    case 136:  // fnabs
      c.fpr[rd] = F64(Bits(c.fpr[rb]) | 0x8000000000000000ull);
      break;
    case 264:  // fabs
      c.fpr[rd] = F64(Bits(c.fpr[rb]) & 0x7FFFFFFFFFFFFFFFull);
      break;
    case 64:  // mcrfs
    {
      const uint32_t shift = CrShift(ra >> 2);
      const uint32_t field = (c.fpscr >> shift) & 0xF;
      c.fpscr &= ~((0xFu << shift) & 0x83F80700u);
      ppc_fpscr_updated(&c);
      c.cr = (c.cr & ~(0xFu << CrShift(rd >> 2))) | (field << CrShift(rd >> 2));
      goto done;
    }
    case 134:  // mtfsfi
    {
      const uint32_t shift = CrShift(rd >> 2);
      c.fpscr = (c.fpscr & ~(0xFu << shift)) | (((raw >> 12) & 0xF) << shift);
      ppc_fpscr_updated(&c);
      cycles = 3;
      break;
    }
    case 583:  // mffs
      c.fpr[rd] = F64(0xFFF8000000000000ull | c.fpscr);
      break;
    case 711:  // mtfsf
    {
      const uint32_t fm = (raw >> 17) & 0xFF;
      uint32_t mask = 0;
      for (uint32_t i = 0; i < 8; ++i)
        if (fm & (1u << i))
          mask |= 0xFu << (i * 4);
      c.fpscr = (c.fpscr & ~mask) | (static_cast<uint32_t>(Bits(c.fpr[rb])) & mask);
      ppc_fpscr_updated(&c);
      cycles = 3;
      break;
    }
    default:
      illegal();
      goto done;
    }
    if (rc && !c.exception)
      SetCr1(c);
    break;
  }

  default:
    illegal();
    break;
  }

done:
  if (c.exception)
  {
    // The helper already moved pc to the vector.
    c.exception = 0;
    c.program_exception = 0;
    EnterException();
  }
  else
  {
    c.pc = next;
  }
  return cycles;
}
}  // namespace dualize
