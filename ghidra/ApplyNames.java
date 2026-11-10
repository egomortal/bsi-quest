import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*; import ghidra.program.model.symbol.*; import ghidra.program.model.address.*;
import ghidra.program.model.data.*; import ghidra.app.util.NamespaceUtils; import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*; import java.util.*;
public class ApplyNames extends GhidraScript {
  Namespace ns(String path) throws Exception {
    return NamespaceUtils.createNamespaceHierarchy(path, null, currentProgram, SourceType.IMPORTED); }
  void name(Address a, String full, boolean isFunc) throws Exception {
    String n=full, p=null; int i=full.lastIndexOf("::");
    if (i>0) { p=full.substring(0,i); n=full.substring(i+2); }
    Namespace space = p==null? currentProgram.getGlobalNamespace() : ns(p);
    if (isFunc) {
      Function f = getFunctionAt(a);
      if (f==null) { new CreateFunctionCmd(a).applyTo(currentProgram); f=getFunctionAt(a); }
      if (f!=null) { f.setParentNamespace(space); f.setName(n, SourceType.IMPORTED); return; }
    }
    Symbol s = currentProgram.getSymbolTable().createLabel(a, n, space, SourceType.IMPORTED); s.setPrimary();
  }
  public void run() throws Exception {
    int ok=0, bad=0, reset=0;
    SymbolTable st=currentProgram.getSymbolTable();
    List<Symbol> imp=new ArrayList<>();
    for (Symbol sy: st.getAllSymbols(true)) if (sy.getSource()==SourceType.IMPORTED && (sy.getSymbolType()==SymbolType.FUNCTION || sy.getSymbolType()==SymbolType.LABEL)) imp.add(sy);
    for (Symbol sy: imp) { try {
      if (sy.getSymbolType()==SymbolType.FUNCTION) { Function f=getFunctionAt(sy.getAddress()); f.setParentNamespace(currentProgram.getGlobalNamespace()); f.setName(null, SourceType.DEFAULT); }
      else sy.delete(); reset++; } catch(Exception e){} }
    println("RESET "+reset);
    for (String l : java.nio.file.Files.readAllLines(new File("/home/claude/bsi/names_all.tsv").toPath())) {
      String[] t=l.split("\t"); try { name(toAddr(Long.parseLong(t[0],16)), t[1], true); ok++; } catch(Exception e){ bad++; } }
    for (String l : java.nio.file.Files.readAllLines(new File("/home/claude/bsi/labels_all.tsv").toPath())) {
      String[] t=l.split("\t"); try { name(toAddr(Long.parseLong(t[0],16)), t[1], false); ok++; } catch(Exception e){ bad++; } }
    // class structs with parent nested at offset 0
    DataTypeManager dtm=currentProgram.getDataTypeManager(); CategoryPath cp=new CategoryPath("/BSI_Classes");
    Map<String,String[]> rows=new LinkedHashMap<>();
    for (String l : java.nio.file.Files.readAllLines(new File("/home/claude/bsi/classes_struct.tsv").toPath())) { String[] t=l.split("\t",-1); rows.put(t[0],t); }
    Map<String,Structure> made=new HashMap<>();
    for (String n: rows.keySet()) { int sz=Integer.parseInt(rows.get(n)[1]); made.put(n,(Structure)dtm.addDataType(new StructureDataType(cp,n,sz),DataTypeConflictHandler.REPLACE_HANDLER)); }
    int nested=0;
    for (String n: rows.keySet()) { String p=rows.get(n)[2]; Structure s=made.get(n), ps=made.get(p);
      if (ps!=null && ps.getLength()<=s.getLength() && !p.equals(n)) { try { s.replaceAtOffset(0, ps, ps.getLength(), "Super", "parent class "+p); nested++; } catch(Exception e){} } }
    println("APPLIED "+ok+" FAILED "+bad+" STRUCTS "+made.size()+" NESTED "+nested);
  }
}
