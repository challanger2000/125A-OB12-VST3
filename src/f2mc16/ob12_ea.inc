// OB-12 addition: complete effective-address decoding for the 0x70-0x7F instruction tables.
// Upstream implements only the addressing modes its own drivers needed. Anything it does
// not handle lands here. Semantics follow the F2MC-16LX Programming Manual, CM44-00201.

struct f2mc16_ea
{
	bool reg;      // mode 0-7: operand is a register
	int index;     // register number for register mode
	u32 address;   // 24-bit address for memory modes
	int length;    // extra instruction bytes used by the mode
};

// size is the operand size in bytes, needed for the post-increment modes.
static f2mc16_ea ob12_no_ea() { return f2mc16_ea{ true, 0, 0, 0 }; }

f2mc16_ea f2mc16_device::ea_decode(u8 mode, int size)
{
	f2mc16_ea ea = ob12_no_ea();
	const u32 code = (m_pcb << 16);
	mode &= 0x1f;
	ea.reg = mode < 8;
	ea.index = mode & 7;
	if (ea.reg)
	{
		return ea;
	}
	if (mode < 0x0c) // @RWi
	{
		ea.address = getRWbank(mode & 3, read_rwX(mode & 3));
	}
	else if (mode < 0x10) // @RWi+
	{
		u16 value = read_rwX(mode & 3);
		ea.address = getRWbank(mode & 3, value);
		write_rwX(mode & 3, value + size);
	}
	else if (mode < 0x18) // @RWi + disp8
	{
		u16 value = read_rwX(mode & 7) + s8(read_8(code | u16(m_pc + 2)));
		ea.address = getRWbank(mode & 7, value);
		ea.length = 1;
	}
	else if (mode < 0x1c) // @RWi + disp16
	{
		u16 value = read_rwX(mode & 3) + read_16(code | u16(m_pc + 2));
		ea.address = getRWbank(mode & 3, value);
		ea.length = 2;
	}
	else if (mode < 0x1e) // @RW0 + RW7, @RW1 + RW7
	{
		u16 value = read_rwX(mode & 1) + read_rwX(7);
		ea.address = getRWbank(mode & 1, value);
	}
	else if (mode == 0x1e) // @PC + disp16, relative to the instruction address + 4
	{
		ea.address = code | u16(m_pc + 4 + read_16(code | u16(m_pc + 2)));
		ea.length = 2;
		m_prefix_valid = false;
	}
	else // addr16
	{
		ea.address = getRWbank(0, read_16(code | u16(m_pc + 2)));
		ea.length = 2;
	}
	return ea;
}

u8 f2mc16_device::ea_r8(const f2mc16_ea &ea) { return ea.reg ? read_rX(ea.index) : read_8(ea.address); }
u16 f2mc16_device::ea_r16(const f2mc16_ea &ea) { return ea.reg ? read_rwX(ea.index) : read_16(ea.address); }
u32 f2mc16_device::ea_r32(const f2mc16_ea &ea) { return ea.reg ? read_rlX(ea.index >> 1) : read_32(ea.address); }
void f2mc16_device::ea_w8(const f2mc16_ea &ea, u8 data) { if (ea.reg) write_rX(ea.index, data); else write_8(ea.address, data); }
void f2mc16_device::ea_w16(const f2mc16_ea &ea, u16 data) { if (ea.reg) write_rwX(ea.index, data); else write_16(ea.address, data); }
void f2mc16_device::ea_w32(const f2mc16_ea &ea, u32 data) { if (ea.reg) write_rlX(ea.index >> 1, data); else write_32(ea.address, data); }

void f2mc16_device::ob12_logic_flags8(u8 value) { setNZ_8(value); m_ps &= ~F_V; }
void f2mc16_device::ob12_logic_flags16(u16 value) { setNZ_16(value); m_ps &= ~F_V; }
void f2mc16_device::ob12_logic_flags32(u32 value) { setNZ_32(value); m_ps &= ~F_V; }

// Byte result into A with bits 8-15 cleared ("Z" in the manual's LH column).
void f2mc16_device::ob12_acc8(u8 value) { m_acc = (m_acc & 0xffff0000) | value; }
void f2mc16_device::ob12_acc16(u16 value) { m_acc = (m_acc & 0xffff0000) | value; }

// MOVEA of a register yields the register's own address (manual 9.1.69).
u16 f2mc16_device::ob12_ea_address(const f2mc16_ea &ea)
{
	if (ea.reg)
		return 0x180 + ((m_ps >> 8) & 0x1f) * 0x10 + ea.index * 2;
	return ea.address & 0xffff;
}

bool f2mc16_device::ea_op(u8 table, u8 operand)
{
	const u32 code = m_pcb << 16;
	const int op = operand >> 5;
	static const u8 sizes[16] = { 4, 4, 1, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 2, 1, 2 };
	int size = sizes[table & 15];
	if (table == 0x70 && (op == 2 || op == 7)) size = op == 2 ? 2 : 1; // CWBNE, CBNE
	if (table == 0x71 && op >= 6) size = 1;
	if (table == 0x78) size = (op & 1) ? 2 : 1;
	f2mc16_ea ea = ea_decode(operand, size);
	u16 next = m_pc + 2 + ea.length;
	u8 a8 = m_acc & 0xff;
	u16 a16 = m_acc & 0xffff;
	m_rmw = true;

	switch (table)
	{
	case 0x70:
		switch (op)
		{
		case 0: m_acc = doADD_32(m_acc, ea_r32(ea)); break;
		case 1: m_acc = doSUB_32(m_acc, ea_r32(ea)); break;
		case 2:
		{
			u16 value = ea_r16(ea);
			u16 imm = read_16(code | next);
			s8 rel = read_8(code | u16(next + 2));
			doCMP_16(value, imm);
			next += 3;
			if (value != imm) next += rel;
			break;
		}
		case 3: doCMP_32(m_acc, ea_r32(ea)); break;
		case 4: m_acc &= ea_r32(ea); ob12_logic_flags32(m_acc); break;
		case 5: m_acc |= ea_r32(ea); ob12_logic_flags32(m_acc); break;
		case 6: m_acc ^= ea_r32(ea); ob12_logic_flags32(m_acc); break;
		case 7:
		{
			u8 value = ea_r8(ea);
			u8 imm = read_8(code | next);
			s8 rel = read_8(code | u16(next + 1));
			doCMP_8(value, imm);
			next += 2;
			if (value != imm) next += rel;
			break;
		}
		}
		break;

	case 0x71:
		switch (op)
		{
		case 0: { u32 target = ea_r32(ea); m_pcb = target >> 16; next = target; break; }
		case 1:
		{
			u32 target = ea_r32(ea);
			push_16(m_pcb);
			push_16(next);
			m_pcb = target >> 16;
			next = target;
			break;
		}
		case 2: ea_w32(ea, doINC_32(ea_r32(ea))); break;
		case 3: ea_w32(ea, doDEC_32(ea_r32(ea))); break;
		case 4: m_acc = ea_r32(ea); setNZ_32(m_acc); break;
		case 5: ea_w32(ea, m_acc); setNZ_32(m_acc); break;
		case 6: ea_w8(ea, read_8(code | next)); next += 1; break;
		case 7: m_acc = (m_acc << 16) | ob12_ea_address(ea); break;
		}
		break;

	case 0x72:
		switch (op)
		{
		case 0:
		{
			u8 value = ea_r8(ea);
			u8 carry = (m_ps & F_C) ? 1 : 0;
			m_ps = (m_ps & ~F_C) | ((value & 0x80) ? F_C : 0);
			value = (value << 1) | carry;
			ea_w8(ea, value);
			setNZ_8(value);
			break;
		}
		case 1:
		{
			u8 value = ea_r8(ea);
			u8 carry = (m_ps & F_C) ? 0x80 : 0;
			m_ps = (m_ps & ~F_C) | ((value & 1) ? F_C : 0);
			value = (value >> 1) | carry;
			ea_w8(ea, value);
			setNZ_8(value);
			break;
		}
		case 2: ea_w8(ea, doINC_8(ea_r8(ea))); break;
		case 3: ea_w8(ea, doDEC_8(ea_r8(ea))); break;
		case 4: { u8 value = ea_r8(ea); m_acc = (m_acc << 16) | value; setNZ_8(value); break; }
		case 5: ea_w8(ea, a8); setNZ_8(a8); break;
		case 6:
		{
			u8 value = ea_r8(ea);
			m_acc = (m_acc << 16) | u16(s16(s8(value)));
			setNZ_8(value);
			break;
		}
		case 7: { u8 value = ea_r8(ea); ea_w8(ea, a8); ob12_acc8(value); break; }
		}
		break;

	case 0x73:
		switch (op)
		{
		case 0: next = ea_r16(ea); break;
		case 1: { u16 target = ea_r16(ea); push_16(next); next = target; break; }
		case 2: ea_w16(ea, doINC_16(ea_r16(ea))); break;
		case 3: ea_w16(ea, doDEC_16(ea_r16(ea))); break;
		case 4: { u16 value = ea_r16(ea); m_acc = (m_acc << 16) | value; setNZ_16(value); break; }
		case 5: ea_w16(ea, a16); setNZ_16(a16); break;
		case 6: ea_w16(ea, read_16(code | next)); next += 2; break;
		case 7: { u16 value = ea_r16(ea); ea_w16(ea, a16); ob12_acc16(value); break; }
		}
		break;

	case 0x74:
		switch (op)
		{
		case 0: ob12_acc8(doADD_8(a8, ea_r8(ea))); break;
		case 1: ob12_acc8(doSUB_8(a8, ea_r8(ea))); break;
		case 2: ob12_acc8(doADDC_8(a8, ea_r8(ea))); break;
		case 3: doCMP_8(a8, ea_r8(ea)); break;
		case 4: m_acc &= 0xffffff00 | ea_r8(ea); ob12_logic_flags8(m_acc & 0xff); break;
		case 5: m_acc |= ea_r8(ea); ob12_logic_flags8(m_acc & 0xff); break;
		case 6: m_acc ^= ea_r8(ea); ob12_logic_flags8(m_acc & 0xff); break;
		case 7:
		{
			u8 value = ea_r8(ea) - 1;
			ea_w8(ea, value);
			setNZ_8(value);
			m_ps &= ~F_V;
			s8 rel = read_8(code | next);
			next += 1;
			if (value) next += rel;
			break;
		}
		}
		break;

	case 0x75:
		switch (op)
		{
		case 0: ea_w8(ea, doADD_8(ea_r8(ea), a8)); break;
		case 1: ea_w8(ea, doSUB_8(ea_r8(ea), a8)); break;
		case 2: ob12_acc8(doSUBC_8(a8, ea_r8(ea))); break;
		case 3: ea_w8(ea, doSUB_8(0, ea_r8(ea))); break;
		case 4: { u8 value = ea_r8(ea) & a8; ea_w8(ea, value); ob12_logic_flags8(value); break; }
		case 5: { u8 value = ea_r8(ea) | a8; ea_w8(ea, value); ob12_logic_flags8(value); break; }
		case 6: { u8 value = ea_r8(ea) ^ a8; ea_w8(ea, value); ob12_logic_flags8(value); break; }
		case 7: { u8 value = ~ea_r8(ea); ea_w8(ea, value); ob12_logic_flags8(value); break; }
		}
		break;

	case 0x76:
		switch (op)
		{
		case 0: ob12_acc16(doADD_16(a16, ea_r16(ea))); break;
		case 1: ob12_acc16(doSUB_16(a16, ea_r16(ea))); break;
		case 2: ob12_acc16(doADDC_16(a16, ea_r16(ea))); break;
		case 3: doCMP_16(a16, ea_r16(ea)); break;
		case 4: m_acc &= 0xffff0000 | ea_r16(ea); ob12_logic_flags16(m_acc & 0xffff); break;
		case 5: m_acc |= ea_r16(ea); ob12_logic_flags16(m_acc & 0xffff); break;
		case 6: m_acc ^= ea_r16(ea); ob12_logic_flags16(m_acc & 0xffff); break;
		case 7:
		{
			u16 value = ea_r16(ea) - 1;
			ea_w16(ea, value);
			setNZ_16(value);
			m_ps &= ~F_V;
			s8 rel = read_8(code | next);
			next += 1;
			if (value) next += rel;
			break;
		}
		}
		break;

	case 0x77:
		switch (op)
		{
		case 0: ea_w16(ea, doADD_16(ea_r16(ea), a16)); break;
		case 1: ea_w16(ea, doSUB_16(ea_r16(ea), a16)); break;
		case 2: ob12_acc16(doSUBC_16(a16, ea_r16(ea))); break;
		case 3: ea_w16(ea, doSUB_16(0, ea_r16(ea))); break;
		case 4: { u16 value = ea_r16(ea) & a16; ea_w16(ea, value); ob12_logic_flags16(value); break; }
		case 5: { u16 value = ea_r16(ea) | a16; ea_w16(ea, value); ob12_logic_flags16(value); break; }
		case 6: { u16 value = ea_r16(ea) ^ a16; ea_w16(ea, value); ob12_logic_flags16(value); break; }
		case 7: { u16 value = ~ea_r16(ea); ea_w16(ea, value); ob12_logic_flags16(value); break; }
		}
		break;

	case 0x78:
		switch (op)
		{
		case 0: ob12_acc16(u16(a8) * ea_r8(ea)); break;
		case 1: m_acc = u32(a16) * ea_r16(ea); break;
		case 2: ob12_acc16(u16(s16(s8(a8)) * s16(s8(ea_r8(ea))))); break;
		case 3: m_acc = u32(s32(s16(a16)) * s32(s16(ea_r16(ea)))); break;
		case 4:
		{
			u8 divisor = ea_r8(ea);
			m_ps &= ~(F_V | F_C);
			if (!divisor) { m_ps |= F_V | F_C; break; }
			u16 quotient = a16 / divisor;
			if (quotient > 0xff) { m_ps |= F_V; break; }
			ea_w8(ea, a16 % divisor);
			ob12_acc8(quotient);
			break;
		}
		case 5:
		{
			u16 divisor = ea_r16(ea);
			m_ps &= ~(F_V | F_C);
			if (!divisor) { m_ps |= F_V | F_C; break; }
			u32 quotient = m_acc / divisor;
			if (quotient > 0xffff) { m_ps |= F_V; break; }
			ea_w16(ea, m_acc % divisor);
			ob12_acc16(quotient);
			break;
		}
		case 6:
		{
			s16 divisor = s8(ea_r8(ea));
			m_ps &= ~(F_V | F_C);
			if (!divisor) { m_ps |= F_V | F_C; break; }
			s32 quotient = s16(a16) / divisor;
			if (quotient > 127 || quotient < -128) { m_ps |= F_V; break; }
			ea_w8(ea, s16(a16) % divisor);
			ob12_acc8(u8(quotient));
			break;
		}
		case 7:
		{
			s32 divisor = s16(ea_r16(ea));
			m_ps &= ~(F_V | F_C);
			if (!divisor) { m_ps |= F_V | F_C; break; }
			s64 quotient = s64(s32(m_acc)) / divisor;
			if (quotient > 32767 || quotient < -32768) { m_ps |= F_V; break; }
			ea_w16(ea, u16(s64(s32(m_acc)) % divisor));
			ob12_acc16(u16(quotient));
			break;
		}
		}
		break;

	case 0x79: write_rwX(op, ob12_ea_address(ea)); break;
	case 0x7a: { u8 value = ea_r8(ea); write_rX(op, value); setNZ_8(value); break; }
	case 0x7b: { u16 value = ea_r16(ea); write_rwX(op, value); setNZ_16(value); break; }
	case 0x7c: { u8 value = read_rX(op); ea_w8(ea, value); setNZ_8(value); break; }
	case 0x7d: { u16 value = read_rwX(op); ea_w16(ea, value); setNZ_16(value); break; }
	case 0x7e: { u8 value = ea_r8(ea); ea_w8(ea, read_rX(op)); write_rX(op, value); break; }
	case 0x7f: { u16 value = ea_r16(ea); ea_w16(ea, read_rwX(op)); write_rwX(op, value); break; }

	default:
		m_rmw = false;
		return false;
	}

	m_rmw = false;
	m_prefix_valid = false;
	m_pc = next;
	m_icount -= 6;
	return true;
}
