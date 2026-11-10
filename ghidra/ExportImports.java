import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*; import ghidra.program.model.symbol.*;
import java.io.*;
public class ExportImports extends GhidraScript {
  public void run() throws Exception {
    PrintWriter w=new PrintWriter(new FileWriter("/home/claude/bsi/import_sigs.tsv"));
    for (Function f: currentProgram.getFunctionManager().getExternalFunctions()) {
      ExternalLocation el=f.getExternalLocation();
      int bytes=0; for (Parameter p: f.getParameters()) bytes+= Math.max(4,(p.getLength()+3)&~3);
      w.println(el.getLibraryName()+"\t"+f.getName()+"\t"+f.getParameterCount()+"\t"+bytes+"\t"+f.getCallingConventionName()+"\t"+f.hasVarArgs()+"\t"+f.getReturnType().getLength()+"\t"+f.getReturnType().getName());
    }
    w.close(); println("done");
  }
}
