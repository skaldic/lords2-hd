// Lists every instruction operand that bakes the 640-byte frame pitch in: a register-relative
// displacement, or an immediate added to/subtracted from/loaded into a register, equal to
// rows * 640 + column with rows 1..16 and column -2..2 (the blitters address "the pixel below"
// as [EDI+0x280], fifteen rows down as [EDI+0x2580], and so on).
// Output, one per line: address, offset of the 32-bit field in the instruction, value, rows,
// column, function, instruction text.
// Usage (headless): -postScript PitchSites.java <out file>
//@category Lords2

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class PitchSites extends GhidraScript {

	private static final int PITCH = 640;

	@Override
	protected void run() throws Exception {
		File out = new File(getScriptArgs()[0]);
		int count = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			InstructionIterator it = currentProgram.getListing().getInstructions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Instruction ins = it.next();
				String mnem = ins.getMnemonicString();
				for (int op = 0; op < ins.getNumOperands(); op++) {
					int type = ins.getOperandType(op);
					boolean memory = (type & OperandType.DYNAMIC) != 0; // [reg + disp]
					boolean immediate = (type & OperandType.SCALAR) != 0 && (type & OperandType.ADDRESS) == 0
						&& (mnem.equals("ADD") || mnem.equals("SUB") || mnem.equals("LEA") || mnem.equals("MOV"));
					if (!memory && !immediate) {
						continue;
					}
					for (Object o : ins.getOpObjects(op)) {
						if (!(o instanceof Scalar)) {
							continue;
						}
						long v = ((Scalar) o).getSignedValue();
						long a = Math.abs(v);
						long rows = (a + 2) / PITCH;
						long col = a - rows * PITCH;
						if (rows < 1 || rows > 16 || col < -2 || col > 2) {
							continue;
						}
						if (immediate && mnem.equals("MOV") && !ins.getDefaultOperandRepresentation(0).matches("E[A-Z]{2}")) {
							continue; // a stored number, not pointer arithmetic
						}
						byte[] b = ins.getBytes();
						int at = -1;
						for (int i = 0; i + 4 <= b.length; i++) {
							long x = (b[i] & 0xffL) | (b[i + 1] & 0xffL) << 8 | (b[i + 2] & 0xffL) << 16 | ((long) b[i + 3]) << 24;
							if (x == v) {
								at = i;
							}
						}
						if (at < 0) {
							continue;
						}
						Function f = getFunctionContaining(ins.getAddress());
						w.printf("%s\t%d\t%d\t%d\t%d\t%s\t%s%n", ins.getAddress(), at, v, v < 0 ? -rows : rows,
							v < 0 ? -col : col, f == null ? "" : f.getEntryPoint().toString(), ins);
						count++;
					}
				}
			}
		}
		println("PitchSites: " + count + " operands to " + out);
	}
}
