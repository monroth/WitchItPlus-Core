// Local trusted parser guard for Windows PowerShell 5.1 ConvertFrom-Json.
using System; using System.Collections.Generic; using System.Globalization;
namespace WitchItUpdater {
 public static class JsonGuard {
  sealed class Reader {
   readonly string s; int i,nodes;
   internal Reader(string text){s=text;}
   void Fail(){throw new FormatException("Invalid, duplicate or oversized JSON");}
   void WS(){while(i<s.Length && " \t\r\n".IndexOf(s[i])>=0)i++;}
   char Take(){if(i>=s.Length)Fail();return s[i++];}
   void Want(char c){if(Take()!=c)Fail();}
   string Text(){Want('"');var b=new System.Text.StringBuilder();while(true){char c=Take();if(c=='"')return b.ToString();if(c<32)Fail();if(c=='\\'){c=Take();switch(c){case '"':case '\\':case '/':b.Append(c);break;case 'b':b.Append('\b');break;case 'f':b.Append('\f');break;case 'n':b.Append('\n');break;case 'r':b.Append('\r');break;case 't':b.Append('\t');break;case 'u':if(i+4>s.Length)Fail();ushort v;if(!ushort.TryParse(s.Substring(i,4),NumberStyles.HexNumber,CultureInfo.InvariantCulture,out v))Fail();i+=4;b.Append((char)v);break;default:Fail();break;}}else b.Append(c);if(b.Length>65536)Fail();}}
   void Literal(string value){foreach(char c in value)Want(c);}
   void Number(){int start=i;if(i<s.Length&&s[i]=='-')i++;if(i>=s.Length)Fail();if(s[i]=='0')i++;else {if(s[i]<'1'||s[i]>'9')Fail();while(i<s.Length&&char.IsDigit(s[i]))i++;}if(i<s.Length&&s[i]=='.'){i++;int begin=i;while(i<s.Length&&char.IsDigit(s[i]))i++;if(begin==i)Fail();}if(i<s.Length&&(s[i]=='e'||s[i]=='E')){i++;if(i<s.Length&&(s[i]=='+'||s[i]=='-'))i++;int begin=i;while(i<s.Length&&char.IsDigit(s[i]))i++;if(begin==i)Fail();}double d;if(!double.TryParse(s.Substring(start,i-start),NumberStyles.Float,CultureInfo.InvariantCulture,out d)||double.IsNaN(d)||double.IsInfinity(d))Fail();}
   void Value(int depth){WS();if(depth>20||++nodes>100000||i>=s.Length)Fail();char c=s[i];if(c=='{'){i++;WS();var keys=new HashSet<string>(StringComparer.OrdinalIgnoreCase);if(i<s.Length&&s[i]=='}'){i++;return;}while(true){WS();if(!keys.Add(Text()))Fail();WS();Want(':');Value(depth+1);WS();c=Take();if(c=='}')return;if(c!=',')Fail();}}else if(c=='['){i++;WS();if(i<s.Length&&s[i]==']'){i++;return;}while(true){Value(depth+1);WS();c=Take();if(c==']')return;if(c!=',')Fail();}}else if(c=='"')Text();else if(c=='t')Literal("true");else if(c=='f')Literal("false");else if(c=='n')Literal("null");else Number();}
   internal void Read(){Value(0);WS();if(i!=s.Length)Fail();}
  }
  public static void Validate(string text){if(text==null||text.Length>4194304)throw new FormatException("JSON bound");new Reader(text).Read();}
 }
 public static class PackageIds {
  public static bool IsPublic(string id){return String.Equals(id,"Core",StringComparison.Ordinal)||String.Equals(id,"BaseWorldPlus",StringComparison.Ordinal)||String.Equals(id,"FillAPotPlus",StringComparison.Ordinal)||String.Equals(id,"MatchRulesPlusPlus",StringComparison.Ordinal);}
  public static string Bundle(string id){switch(id){case "Core":return "core";case "BaseWorldPlus":return "editor-tweaks";case "FillAPotPlus":return "fillapot-plus";case "MatchRulesPlusPlus":return "matchrules-plus";default:throw new FormatException("Unknown/noncanonical package ID");}}
 }

}
