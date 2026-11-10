import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*; import ghidra.program.model.symbol.*; import ghidra.program.model.address.*;
import java.io.*; import java.util.*;
public class ExportBodies extends GhidraScript {
  public void run() throws Exception {
    PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter("/home/claude/bsi/bodies.tsv")));
    Listing L=currentProgram.getListing(); ReferenceManager rm=currentProgram.getReferenceManager();
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      StringBuilder rs=new StringBuilder();
      for (AddressRange r : f.getBody()) { if (rs.length()>0) rs.append(","); rs.append(r.getMinAddress()+"-"+r.getMaxAddress()); }
      StringBuilder sw=new StringBuilder();
      InstructionIterator it=L.getInstructions(f.getBody(),true);
      while (it.hasNext()) { Instruction ins=it.next();
        FlowType ft=ins.getFlowType();
        if (ft.isJump() && ft.isComputed()) {
          sw.append(ins.getAddress()+":");
          for (Reference r: rm.getReferencesFrom(ins.getAddress())) if (r.getReferenceType().isFlow()) sw.append(r.getToAddress()+"/");
          sw.append(";"); } }
      w.println(f.getEntryPoint()+"\t"+rs+"\t"+sw+"\t"+f.getCallingConventionName()+"\t"+f.getStackPurgeSize());
    }
    w.close(); println("done");
  }
}
