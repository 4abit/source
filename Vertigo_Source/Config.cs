using System.IO;
using System.Text.Json;

namespace Vertigo;

public sealed record Config
{
    public bool Esp {get;init;} = true;
    public bool Boxes {get;init;} = true;
    public bool Corners {get;init;} = true;
    public bool Skeleton {get;init;} = true;
    public bool Names {get;init;} = true;
    public bool Health {get;init;} = true;
    public bool Armor {get;init;}
    public bool Money {get;init;} = true;
    public bool Distance {get;init;} = true;
    public bool Teammates {get;init;}
    public bool Steps {get;init;} = true;
    public bool HeadDot {get;init;}
    public double EspRange {get;init;} = 150;
    public double LineWidth {get;init;} = 1.2;
    public double EspOpacity {get;init;} = .95;
    public int Rig {get;init;}
    public bool Aim {get;init;}
    public bool AimFireOnly {get;init;} = true;
    public bool SpottedOnly {get;init;} = true;
    public bool FovCircle {get;init;} = true;
    public int AimKey {get;init;} = 2;
    public int AimBone {get;init;}
    public double AimFov {get;init;} = 3;
    public double AimSmooth {get;init;} = 100;
    public bool Rcs {get;init;}
    public double RcsX {get;init;} = 1;
    public double RcsY {get;init;} = 1;
    public double RcsSmooth {get;init;} = 18;
    public int RcsStart {get;init;} = 2;
    public bool Trigger {get;init;}
    public int TriggerKey {get;init;} = 0x12;
    public double TriggerDelay {get;init;} = 90;
    public double TriggerInterval {get;init;} = 140;
    public bool TriggerSpotted {get;init;} = true;
    public bool TriggerStationary {get;init;} = true;
    public bool Bhop {get;init;}
    public bool Strafe {get;init;}
    public bool AutoForward {get;init;}
    public int MoveKey {get;init;} = 0x20;
    public bool Bomb {get;init;} = true;
    public bool Spectators {get;init;} = true;
    public bool Watermark {get;init;} = true;
    public bool WatermarkNick {get;init;} = true;
    public bool WatermarkFps {get;init;} = true;
    public bool WatermarkClock {get;init;} = true;
    public bool WatermarkMap {get;init;}
    public bool Speedometer {get;init;} = true;
    public bool CaptureExclude {get;init;}
    public string NickOverride {get;init;} = "";
    public int DrawFps {get;init;} = 120;
    public double MouseYaw {get;init;} = .022;
    public double MousePitch {get;init;} = .022;

    public Config Sanitize()
    {
        double C(double x,double lo,double hi,double fallback) => double.IsFinite(x)?Math.Clamp(x,lo,hi):fallback;
        int K(int k,int fallback) => k is >0 and <255 && k!=0x2d && k!=0x23 ? k:fallback;
        return this with { EspRange=C(EspRange,10,500,150), LineWidth=C(LineWidth,.5,3,1.2),
            EspOpacity=C(EspOpacity,.2,1,.95),Rig=Math.Clamp(Rig,0,2),AimBone=Math.Clamp(AimBone,0,2),
            AimFov=C(AimFov,.2,15,3),AimSmooth=C(AimSmooth,1,500,100),
            RcsX=C(RcsX,0,1.5,1),RcsY=C(RcsY,0,1.5,1),RcsSmooth=C(RcsSmooth,0,100,18),RcsStart=Math.Clamp(RcsStart,1,10),
            TriggerDelay=C(TriggerDelay,0,500,90),TriggerInterval=C(TriggerInterval,50,1000,140),
            AimKey=K(AimKey,2),TriggerKey=K(TriggerKey,0x12),MoveKey=K(MoveKey,0x20),
            MouseYaw=C(MouseYaw,.001,.2,.022),MousePitch=C(MousePitch,.001,.2,.022),
            DrawFps=Math.Clamp(DrawFps,30,240),NickOverride=(NickOverride??"")[..Math.Min((NickOverride??"").Length,32)]};
    }
}

public static class Profiles
{
    public static string Root {get;} = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Vertigo");
    public static string DirectoryPath => Path.Combine(Root,"configs");
    static readonly JsonSerializerOptions Json = new(){WriteIndented=true};
    static string FileFor(string name)
    {
        if (string.IsNullOrWhiteSpace(name)||name.Length>40||name.Any(c=>!char.IsLetterOrDigit(c)&&c!='-'&&c!='_'))
            throw new ArgumentException("Имя: буквы, цифры, дефис или подчёркивание (до 40).");
        return Path.Combine(DirectoryPath,name+".json");
    }
    public static void Save(string name,Config c)
    {
        string path=FileFor(name); Directory.CreateDirectory(DirectoryPath);
        File.WriteAllText(path+".tmp",JsonSerializer.Serialize(c.Sanitize(),Json)); File.Move(path+".tmp",path,true);
    }
    public static Config Load(string name) => (JsonSerializer.Deserialize<Config>(File.ReadAllText(FileFor(name)))??new()).Sanitize();
    public static string[] List() => Directory.Exists(DirectoryPath)?Directory.GetFiles(DirectoryPath,"*.json").Select(Path.GetFileNameWithoutExtension).OfType<string>().Order().ToArray():[];
}

public static class Log
{
    static readonly object Gate=new();
    public static void Write(string text)
    {
        lock(Gate) { Directory.CreateDirectory(Profiles.Root); File.AppendAllText(Path.Combine(Profiles.Root,"vertigo.log"),$"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {text}\n"); }
    }
}
