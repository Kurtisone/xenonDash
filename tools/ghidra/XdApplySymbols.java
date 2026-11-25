// Applique au programme courant les entrées produites par tools/ghidra/xd_ghidra.py (fonctions, imports, étiquettes,
// commentaires). Arguments : <entrees.tsv>. Lignes : genre, adresse (hex), nom, taille, commentaire, séparés par des
// tabulations ; genre fn = début de fonction (corps calculé par Ghidra en suivant le flot), inl = idem marquée « inline »
// (aides de sauvegarde des registres), imp = thunk d'import (corps de <taille> octets), lbl = étiquette de donnée.
// Réapplicable : les noms existants sont remplacés, rien n'est supprimé.
//@category XenonDash

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class XdApplySymbols extends GhidraScript {

	private static boolean isCode(String kind) {
		return kind.equals("fn") || kind.equals("inl") || kind.equals("imp");
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("usage : XdApplySymbols.java <entrees.tsv>");
			return;
		}
		List<String[]> rows = new ArrayList<>();
		for (String line : Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8)) {
			if (line.isEmpty() || line.startsWith("#")) {
				continue;
			}
			String[] r = line.split("\t", -1);
			if (r.length >= 3) {
				rows.add(r);
			}
		}
		AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
		Listing listing = currentProgram.getListing();
		SymbolTable st = currentProgram.getSymbolTable();

		// 1) Désassemblage depuis tous les débuts de code connus (en une commande : bien plus rapide qu'adresse par adresse).
		AddressSet starts = new AddressSet();
		for (String[] r : rows) {
			if (isCode(r[0])) {
				starts.add(sp.getAddress(Long.parseLong(r[1], 16)));
			}
		}
		monitor.setMessage("désassemblage de " + starts.getNumAddresses() + " débuts");
		new DisassembleCommand(starts, null, true).applyTo(currentProgram, monitor);

		// 2) Création des fonctions (toutes d'abord : une création peut toucher le nom d'une autre, voir 3).
		int failed = 0;
		monitor.initialize(rows.size());
		for (String[] r : rows) {
			monitor.checkCancelled();
			monitor.incrementProgress(1);
			if (!isCode(r[0])) {
				continue;
			}
			Address a = sp.getAddress(Long.parseLong(r[1], 16));
			if (listing.getFunctionAt(a) != null) {
				continue;
			}
			long size = r.length > 3 && !r[3].isEmpty() ? Long.parseLong(r[3]) : 0;
			AddressSet body = r[0].equals("imp") && size > 0 ? new AddressSet(a, a.add(size - 1)) : null;
			CreateFunctionCmd cmd = new CreateFunctionCmd(r[2], a, body, SourceType.IMPORTED);
			if (!cmd.applyTo(currentProgram, monitor) && ++failed <= 20) {
				printerr("fonction non créée à " + a + " (" + r[2] + ") : " + cmd.getStatusMsg());
			}
		}

		// 3) Noms, « inline », étiquettes, commentaires. Une fonction réduite à un saut devient un « thunk » pour Ghidra,
		// qui affiche alors le nom de sa cible (XGetAVPack, saut vers HalGetCurrentAVPack, s'affichait HalGetCurrentAVPack) :
		// elle redevient une fonction ordinaire avec son propre nom.
		int nf = 0, ni = 0, nl = 0, nc = 0, renamed = 0, unthunked = 0;
		for (String[] r : rows) {
			monitor.checkCancelled();
			Address a = sp.getAddress(Long.parseLong(r[1], 16));
			String kind = r[0];
			String name = r[2];
			String comment = r.length > 4 ? r[4] : "";
			try {
				if (isCode(kind)) {
					Function f = listing.getFunctionAt(a);
					if (f == null) {
						continue;
					}
					if (f.isThunk()) {
						f.setThunkedFunction(null);
						unthunked++;
					}
					if (!f.getName().equals(name)) {
						f.setName(name, SourceType.IMPORTED);
						renamed++;
					}
					f.setInline(kind.equals("inl"));
					if (kind.equals("imp")) {
						ni++;
					}
					else {
						nf++;
					}
				}
				else if (kind.equals("lbl")) {
					Symbol s = st.createLabel(a, name, SourceType.IMPORTED);
					s.setPrimary();
					nl++;
				}
				if (!comment.isEmpty()) {
					setPlateComment(a, comment);
					nc++;
				}
			}
			catch (Exception e) {
				if (++failed <= 20) {
					printerr(a + " (" + name + ") : " + e.getMessage());
				}
			}
		}
		// 4) Appels terminaux : « b fonction » devient CALL_RETURN (travail de l'analyseur « Shared Return Calls », qui ne
		// tourne pas ici). Sans cela le décompilateur perd l'appel : XGetAVPack se lisait « return; ».
		int tail = 0;
		FunctionManager fm = currentProgram.getFunctionManager();
		InstructionIterator it = listing.getInstructions(true);
		while (it.hasNext()) {
			monitor.checkCancelled();
			Instruction ins = it.next();
			FlowType ft = ins.getFlowType();
			if (!ft.isJump() || ft.isConditional() || ft.isComputed() || ft.isCall() ||
				ins.getFlowOverride() != FlowOverride.NONE) {
				continue;
			}
			Address[] flows = ins.getFlows();
			if (flows.length != 1 || fm.getFunctionAt(flows[0]) == null) {
				continue;
			}
			Function self = fm.getFunctionContaining(ins.getAddress());
			if (self != null && self.getEntryPoint().equals(flows[0])) {
				continue; // boucle vers sa propre entrée
			}
			ins.setFlowOverride(FlowOverride.CALL_RETURN);
			tail++;
		}
		println(String.format("fonctions %d, imports %d, étiquettes %d, commentaires %d, renommées %d, " +
			"thunks rendus ordinaires %d, appels terminaux %d, échecs %d",
			nf, ni, nl, nc, renamed, unthunked, tail, failed));
	}
}
