import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
public class ExportFuncs extends GhidraScript {
  public void run() throws Exception {
    PrintWriter w = new PrintWriter(new FileWriter("/home/claude/bsi/functions.tsv"));
    int n=0, named=0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      n++; if (!f.getName().startsWith("FUN_")) named++;
      w.println(f.getEntryPoint()+"\t"+f.getName()+"\t"+f.getBody().getNumAddresses());
    }
    w.close();
    println("FUNCTIONS "+n+" NAMED "+named);
  }
}
