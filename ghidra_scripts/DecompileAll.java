// Decompiles every function into src/, one file per 64 KB of code (lords2_4xxxxx.c), with the
// strings each function references listed above it. The pseudo-C is a reading aid kept local.
// Usage (headless): -postScript DecompileAll.java <output dir>
//@category Lords2

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;

public class DecompileAll extends GhidraScript {

	@Override
	protected void run() throws Exception {
		File outDir = new File(getScriptArgs()[0]);
		outDir.mkdirs();
		DecompInterface decomp = new DecompInterface();
		decomp.openProgram(currentProgram);

		int ok = 0, failed = 0;
		long chunk = -1;
		PrintWriter w = null;
		FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Function f = it.next();
			if (f.isThunk() || f.isExternal()) {
				continue;
			}
			long addr = f.getEntryPoint().getOffset();
			if ((addr >> 16) != chunk) {
				if (w != null) {
					w.close();
				}
				chunk = addr >> 16;
				w = new PrintWriter(new File(outDir, String.format("lords2_%04x0000.c", chunk)), "UTF-8");
				w.println("// Decompiled from Lords2.exe (GOG, 1997-06-05). Ghidra pseudo-C: a reading aid, not source.");
			}
			w.println();
			w.printf("// %08x  %s%n", addr, f.getName());
			for (String s : strings(f)) {
				w.println("//   \"" + s + "\"");
			}
			DecompileResults res = decomp.decompileFunction(f, 60, monitor);
			if (res != null && res.decompileCompleted()) {
				w.print(res.getDecompiledFunction().getC());
				ok++;
			}
			else {
				w.println("// (decompilation failed)");
				failed++;
			}
		}
		if (w != null) {
			w.close();
		}
		decomp.dispose();
		println("Decompiled " + ok + " functions (" + failed + " failed) into " + outDir);
	}

	private Set<String> strings(Function f) {
		Set<String> out = new LinkedHashSet<>();
		AddressIterator ai = f.getBody().getAddresses(true);
		while (ai.hasNext()) {
			Address a = ai.next();
			for (Reference r : getReferencesFrom(a)) {
				Data d = getDataAt(r.getToAddress());
				if (d != null && d.hasStringValue()) {
					out.add(String.valueOf(d.getValue()).replace("\n", "\\n").replace("\r", "\\r"));
				}
			}
		}
		return out;
	}
}
