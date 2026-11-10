import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*; import ghidra.program.model.listing.*;
import ghidra.app.cmd.disassemble.DisassembleCommand; import ghidra.app.cmd.function.CreateFunctionCmd;
public class CreateEntry extends GhidraScript {
  public void run() throws Exception {
    long[] addrs={0xfc789dL,0xfc75ddL};
    for (long x: addrs) { Address a=toAddr(x);
      new DisassembleCommand(a,null,true).applyTo(currentProgram);
      Function f=getFunctionContaining(a);
      if (f!=null && !f.getEntryPoint().equals(a)) println("inside "+f.getName()+" @ "+f.getEntryPoint());
      if (getFunctionAt(a)==null) { boolean ok=new CreateFunctionCmd(a).applyTo(currentProgram); println("create "+a+" "+ok); }
      Function g=getFunctionAt(a); if (g!=null) println(g.getName()+" body "+g.getBody());
    }
  }
}
