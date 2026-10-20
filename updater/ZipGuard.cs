using System; using System.IO; using System.Collections.Generic;
namespace WitchItUpdater {
 public static class ZipGuard {
  static void Need(bool b){if(!b)throw new FormatException("Unsupported or inconsistent ZIP structure");}
  static UInt16 U16(byte[] b,int p){return BitConverter.ToUInt16(b,p);}
  static UInt32 U32(byte[] b,int p){return BitConverter.ToUInt32(b,p);}
  public static void Validate(string path){
   byte[] b=File.ReadAllBytes(path);Need(b.Length>=22&&b.Length<=134217728);int end=b.Length-22;
   Need(U32(b,end)==0x06054b50&&U16(b,end+4)==0&&U16(b,end+6)==0&&U16(b,end+20)==0);
   int count=U16(b,end+10);Need(count==U16(b,end+8)&&count>0&&count<=8192);long size=U32(b,end+12),offset=U32(b,end+16);Need(offset+size==end);int pos=checked((int)offset);var ranges=new List<long[]>();
   for(int i=0;i<count;i++){
    Need(pos+46<=end&&U32(b,pos)==0x02014b50);int flags=U16(b,pos+8),method=U16(b,pos+10),name=U16(b,pos+28);Need((flags==0||flags==2048)&&(method==0||method==8)&&name>0&&U16(b,pos+30)==0&&U16(b,pos+32)==0&&U16(b,pos+34)==0);
    long compressed=U32(b,pos+20),expanded=U32(b,pos+24),local=U32(b,pos+42);Need(expanded<=536870912&&local+30+name<=offset&&pos+46+name<=end);
    int q=checked((int)local);Need(U32(b,q)==0x04034b50&&U16(b,q+6)==flags&&U16(b,q+8)==method&&U32(b,q+14)==U32(b,pos+16)&&U32(b,q+18)==compressed&&U32(b,q+22)==expanded&&U16(b,q+26)==name&&U16(b,q+28)==0);
    for(int n=0;n<name;n++)Need(b[q+30+n]==b[pos+46+n]);long finish=local+30+name+compressed;Need(finish<=offset);ranges.Add(new long[]{local,finish});pos+=46+name;
   }
   Need(pos==end);ranges.Sort((a,c)=>a[0].CompareTo(c[0]));long expected=0;foreach(long[] r in ranges){Need(r[0]==expected);expected=r[1];}Need(expected==offset);
  }
 }
}
