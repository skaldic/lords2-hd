// Lists, for every function, the strings it references. With no symbols, the game's own
// status-log messages ("OK :DD Set resolution.") and file names ("title.pl8") are the
// best anchors for naming functions.
// Usage (headless): -postScript StringRefs.java <output.csv>
//@category Lords2

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class StringRefs extends GhidraScript {

	@Override
	protected void run() throws Exception {
		File out = new File(getScriptArgs()[0]);
		int count = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("func,func_name,from,string_addr,string");
			DataIterator it = currentProgram.getListing().getDefinedData(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Data d = it.next();
				if (!d.hasStringValue()) {
					continue;
				}
				String s = String.valueOf(d.getValue()).replace("\"", "'").replace("\r", "\\r").replace("\n", "\\n");
				for (Reference r : getReferencesTo(d.getAddress())) {
					Address from = r.getFromAddress();
					Function f = getFunctionContaining(from);
					w.printf("%s,%s,%s,%s,\"%s\"%n",
						f == null ? "" : f.getEntryPoint().toString(),
						f == null ? "" : f.getName(),
						from, d.getAddress(), s);
					count++;
				}
			}
		}
		println("Exported " + count + " string references to " + out);
	}
}
