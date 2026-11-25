// Décompile des fonctions du programme courant et écrit le C dans <dossier>/<nom>.c (lancé par tools/ghidra/xd_ghidra.py).
// Arguments : <dossier> <cible>... ; cible = adresse hexadécimale (0x819BE610, 819BE610, sub_819BE610 : la fonction qui la
// contient) ou nom de fonction (XamShowGuideUI).
//@category XenonDash

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;

public class XdDecompile extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage : XdDecompile.java <dossier> <cible>...");
			return;
		}
		Path out = Paths.get(args[0]);
		Files.createDirectories(out);
		DecompInterface di = new DecompInterface();
		di.setOptions(new DecompileOptions());
		di.toggleCCode(true);
		di.toggleSyntaxTree(true);
		di.setSimplificationStyle("decompile");
		if (!di.openProgram(currentProgram)) {
			printerr("décompilateur indisponible : " + di.getLastMessage());
			return;
		}
		AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
		try {
			for (int i = 1; i < args.length; i++) {
				String t = args[i];
				Function f = null;
				String hex = t.toLowerCase().replaceFirst("^(0x|sub_)", "");
				if (hex.matches("[0-9a-f]{1,8}")) {
					f = getFunctionContaining(sp.getAddress(Long.parseLong(hex, 16)));
				}
				if (f == null) {
					List<Function> l = getGlobalFunctions(t);
					if (!l.isEmpty()) {
						f = l.get(0);
					}
				}
				if (f == null) {
					printerr("cible introuvable : " + t);
					continue;
				}
				DecompileResults r = di.decompileFunction(f, 180, monitor);
				if (!r.decompileCompleted()) {
					printerr(f.getName() + " : " + r.getErrorMessage());
					continue;
				}
				Path p = out.resolve(f.getName() + ".c");
				Files.writeString(p, "// " + f.getName() + " @ " + f.getEntryPoint() + " (" +
					f.getBody().getNumAddresses() + " octets)\n" + r.getDecompiledFunction().getC(),
					StandardCharsets.UTF_8);
				println("écrit " + p);
			}
		}
		finally {
			di.dispose();
		}
	}
}
