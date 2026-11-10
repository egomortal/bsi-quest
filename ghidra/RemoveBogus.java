import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*; import ghidra.program.model.listing.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*; import java.util.*;
public class RemoveBogus extends GhidraScript {
  public void run() throws Exception {
    int rm=0, fx=0; List<Address> list=new ArrayList<>();
    for (String l: java.nio.file.Files.readAllLines(new File("/home/claude/bsi/bogusfuncs.txt").toPath())) if(!l.isEmpty()) list.add(toAddr(Long.parseLong(l,16)));
    for (Address a: list) { Function f=getFunctionAt(a); if (f!=null) { currentProgram.getFunctionManager().removeFunction(a); rm++; } }
    for (Address a: list) { Function p=getFunctionContaining(a.subtract(1)); if (p==null) p=getFunctionBefore(a);
      if (p!=null) { try { CreateFunctionCmd.fixupFunctionBody(currentProgram,p,monitor); fx++; } catch(Exception e){} } }
    println("REMOVED "+rm+" FIXED "+fx);
  }
}
