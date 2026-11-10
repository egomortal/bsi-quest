import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*; import java.util.*;
public class ExportGraph extends GhidraScript {
  public void run() throws Exception {
    PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter("/home/claude/bsi/graph.tsv")));
    Listing L = currentProgram.getListing(); ReferenceManager rm = currentProgram.getReferenceManager();
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      if (monitor.isCancelled()) break;
      Set<String> calls = new LinkedHashSet<>(), refs = new LinkedHashSet<>();
      for (Function c : f.getCalledFunctions(monitor)) calls.add(c.getEntryPoint().toString());
      InstructionIterator it = L.getInstructions(f.getBody(), true);
      while (it.hasNext()) { Instruction ins = it.next();
        for (Reference r : rm.getReferencesFrom(ins.getAddress())) {
          if (r.getReferenceType().isFlow()) continue;
          refs.add(r.getToAddress().toString()); } }
      w.println(f.getEntryPoint()+"\t"+f.getName()+"\t"+String.join(",",calls)+"\t"+String.join(",",refs));
    }
    w.close(); println("done");
  }
}
