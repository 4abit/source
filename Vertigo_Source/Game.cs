using System.Diagnostics;
using System.IO;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Vertigo;

public sealed class Offsets
{
    readonly JsonDocument globals,classes;
    public int Build{get;}
    public Offsets(string root)
    {
        globals=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"offsets.json")));
        classes=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"client_dll.json")));
        using var info=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"info.json")));
        Build=info.RootElement.GetProperty("build_number").GetInt32();
    }
    public int G(string key,string module="client.dll")=>globals.RootElement.GetProperty(module).GetProperty(key).GetInt32();
    readonly Dictionary<string,int> cache=[];
    public int F(string cls,string key)
    {
        string id=cls+"."+key;
        if(cache.TryGetValue(id,out int value))return value;
        var field=classes.RootElement.GetProperty("client.dll").GetProperty("classes").GetProperty(cls).GetProperty("fields").GetProperty(key);
        value=field.ValueKind==JsonValueKind.Number?field.GetInt32():field.GetProperty("offset").GetInt32();
        if(value<0||value>131000)throw new InvalidDataException("Invalid schema offset: "+id);
        cache[id]=value;return value;
    }
}

public sealed record Player(long Address,string Name,int Hp,int Armor,int? Money,int Team,Vector3 Feet,Vector3 Head,
    Vector3[] Bones,int Rig,bool Spotted,float Distance,bool Immune);
public sealed record StepMark(Vector3 Position,double Time);
public sealed record BombInfo(Vector3 Position,string Label,float? Remaining=null,float? DefuseRemaining=null,bool Defused=false);
public sealed record Frame
{
    public double Time{get;init;}=Clock.Now;public bool Active{get;init;}public bool Alive{get;init;}
    public bool InputReady{get;init;}public bool Grounded{get;init;}
    public int Pid{get;init;}public int Shots{get;init;}public int Crosshair{get;init;}
    public int LocalTeam{get;init;}
    public long CrosshairPawn{get;init;}public int MoveType{get;init;}
    public Native.Rect Rect{get;init;}public float[] Matrix{get;init;}=[];
    public Vector3 Eye{get;init;}public Vector3 Velocity{get;init;}public Vector2 Angles{get;init;}public Vector2 Punch{get;init;}
    public float Sensitivity{get;init;}=1;public float FovScale{get;init;}=1;public float HFov{get;init;}=90;
    public Player[] Players{get;init;}=[];public string[] Spectators{get;init;}=[];public StepMark[] Steps{get;init;}=[];
    public BombInfo? Bomb{get;init;}public string Nick{get;init;}="player";public string Map{get;init;}="";
    public string Status{get;init;}="Ожидание cs2.exe";public double ReadMs{get;init;}
}
public static class Clock {static readonly Stopwatch Watch=Stopwatch.StartNew();public static double Now=>Watch.Elapsed.TotalSeconds;}
public static class Geometry
{
    public static bool Valid(Vector3 v)=>float.IsFinite(v.X)&&float.IsFinite(v.Y)&&float.IsFinite(v.Z)&&v.LengthSquared()<1e12;
    public static bool Valid(Vector2 v)=>float.IsFinite(v.X)&&float.IsFinite(v.Y);
    public static double Wrap(double a)=>(a%360+540)%360-180;
    public static Vector2 Look(Vector3 from,Vector3 to){var d=to-from;return new((float)(-Math.Atan2(d.Z,Math.Sqrt(d.X*d.X+d.Y*d.Y))*180/Math.PI),(float)(Math.Atan2(d.Y,d.X)*180/Math.PI));}
    public static Vector2? Project(Vector3 v,float[] m,double w,double h)
    {
        if(m.Length!=16||!Valid(v))return null;
        double z=m[12]*v.X+m[13]*v.Y+m[14]*v.Z+m[15];if(!double.IsFinite(z)||z<.01)return null;
        var p=new Vector2((float)((1+(m[0]*v.X+m[1]*v.Y+m[2]*v.Z+m[3])/z)*w*.5),(float)((1-(m[4]*v.X+m[5]*v.Y+m[6]*v.Z+m[7])/z)*h*.5));
        return Valid(p)?p:null;
    }
    public static string CleanName(string name)=>new(name.Where(c=>!char.IsControl(c)).Take(32).ToArray());
    public static string CleanMap(string name)
    {name=name.Replace('\\','/').Split('/').Last().Replace(".vpk","").Replace(".vmap","");foreach(string prefix in new[]{"de_","cs_","ar_","dz_"})if(name.StartsWith(prefix))return name[prefix.Length..];return name.Length<40?name:"";}
    public static readonly (int,int)[][] Links = [
        [(6,5),(5,4),(4,2),(2,0),(5,8),(8,9),(9,10),(5,13),(13,14),(14,15),(0,22),(22,23),(23,24),(0,25),(25,26),(26,27)],
        [(7,6),(6,4),(4,3),(3,2),(2,1),(6,9),(9,10),(10,11),(6,13),(13,14),(14,15),(1,17),(17,18),(18,19),(1,20),(20,21),(21,22)]
    ];
    public static int Head(int rig)=>rig==0?6:7;
}

public sealed class Reader
{
    readonly IGameMemory p;readonly Offsets o;int stride;long entityList;
    readonly Dictionary<int,byte[]> pages=[];
    readonly Dictionary<long,string> names=[];
    readonly Dictionary<long,(int Side,Vector3 Pos)> stepState=[];
    readonly List<StepMark> steps=[];
    double namesAt,extrasAt=-1;string map="";string[] spectators=[];BombInfo? bomb;
    public Reader(IGameMemory memory,Offsets offsets)
    {
        p=memory;o=offsets;
        int build=p.Get<int>(p.Engine+o.G("dwBuildNumber","engine2.dll"));
        if(build!=o.Build)throw new InvalidDataException($"Build mismatch: game {build}, data {o.Build}. Обнови Data.");
    }
    T Get<T>(long address,string cls,string field)where T:unmanaged=>p.Get<T>(address+o.F(cls,field));
    T Field<T>(byte[] data,string cls,string field)where T:unmanaged=>MemoryMarshal.Read<T>(data.AsSpan(o.F(cls,field)));
    long Global(string key)=>p.Ptr(p.Client+o.G(key));
    long Entity(uint handle)
    {
        if(handle==0||handle==uint.MaxValue)return 0;int index=(int)(handle&0x7fff),page=index>>9;
        if(!pages.TryGetValue(page,out var data)){long ptr=p.Ptr(entityList+0x10+page*8);data=Memory.Pointer(ptr)?p.Read(ptr,stride*512):[];pages[page]=data;}
        return data.Length>0?BitConverter.ToInt64(data,(index&0x1ff)*stride):0;
    }
    void Detect(long controller,long pawn)
    {
        uint handle=Get<uint>(controller,"CCSPlayerController","m_hPlayerPawn");
        foreach(int s in new[]{0x70,0x78}){stride=s;pages.Clear();try{if(Entity(handle)==pawn&&Enumerable.Range(1,64).Any(i=>Entity((uint)i)==controller))return;}catch{}}
        stride=0;throw new InvalidDataException("Неподдерживаемая структура списка сущностей");
    }
    string Name(long controller)
    {
        if(!names.TryGetValue(controller,out var name)){name=p.Optional(()=>Geometry.CleanName(p.Text(controller+o.F("CBasePlayerController","m_iszPlayerName"))),"player");names[controller]=name;}
        return name;
    }
    Vector3 Position(long entity)=>Get<Vector3>(Get<long>(entity,"C_BaseEntity","m_pGameSceneNode"),"CGameSceneNode","m_vecAbsOrigin");
    Vector3[] Bones(long node,Vector3 feet)
    {
        return p.Optional(()=>{
            long ptr=p.Ptr(node+o.F("CSkeletonInstance","m_modelState")+0x80);
            var raw=p.Read(ptr,28*32);var values=new Vector3[28];int valid=0;
            for(int i=0;i<28;i++){var v=MemoryMarshal.Read<Vector3>(raw.AsSpan(i*32));if(Geometry.Valid(v)&&Vector3.Distance(v,feet)<140){values[i]=v;valid++;}else values[i]=new(float.NaN);}
            return valid>=16?values:[];
        },Array.Empty<Vector3>());
    }
    static int Rig(Vector3[] bones,Vector3 feet,int configured)
    {
        if(configured>0)return configured-1;if(bones.Length<28)return 0;
        double Score(int r){int head=Geometry.Head(r);int left=r==0?24:19,right=r==0?27:22;
            if(!Geometry.Valid(bones[head])||!Geometry.Valid(bones[left])||!Geometry.Valid(bones[right]))return 1e8;
            double score=Math.Abs(bones[left].Z-feet.Z)+Math.Abs(bones[right].Z-feet.Z);
            if(bones[head].Z<feet.Z+30)score+=1000;
            foreach(var(a,b)in Geometry.Links[r])if(!Geometry.Valid(bones[a])||!Geometry.Valid(bones[b])||Vector3.Distance(bones[a],bones[b])>50)score+=100;
            return score;
        }return Score(1)<Score(0)?1:0;
    }
    public Frame Sample(Config cfg)
    {
        double start=Clock.Now;var(rect,active)=p.Window();
        if(!active)return new(){Pid=p.Pid,Rect=rect,Status="CS2 не в фокусе"};
        long pawn=Global("dwLocalPlayerPawn"),ctrl=Global("dwLocalPlayerController");entityList=Global("dwEntityList");
        if(!Memory.Pointer(pawn)||!Memory.Pointer(ctrl)||!Memory.Pointer(entityList))return new(){Pid=p.Pid,Rect=rect,Status="Зайди на карту"};
        if(stride==0)Detect(ctrl,pawn);pages.Clear();
        if(start-namesAt>.8){names.Clear();namesAt=start;}
        int size=o.F("C_CSPlayerPawn","m_iIDEntIndex")+16;
        var local=p.Read(pawn,size);int hp=Field<int>(local,"C_BaseEntity","m_iHealth");bool alive=hp>0&&Field<byte>(local,"C_BaseEntity","m_lifeState")==0;
        int team=Field<byte>(local,"C_BaseEntity","m_iTeamNum");var origin=Position(pawn);
        var eye=origin+Field<Vector3>(local,"C_BaseModelEntity","m_vecViewOffset");
        var angles=p.Get<Vector2>(p.Client+o.G("dwViewAngles"));var matrix=MemoryMarshal.Cast<byte,float>(p.Read(p.Client+o.G("dwViewMatrix"),64)).ToArray();
        if(!Geometry.Valid(eye)||!Geometry.Valid(angles)||matrix.Any(v=>!float.IsFinite(v))||matrix.All(v=>Math.Abs(v)<.001))throw new InvalidDataException("Невалидные данные камеры");
        float sensitivity=p.Optional(()=>p.Get<float>(Global("dwSensitivity")+o.G("dwSensitivity_sensitivity")),0);
        float scale=Field<float>(local,"C_BasePlayerPawn","m_flFOVSensitivityAdjust");
        var punch=p.Optional(()=>Get<Vector2>(Field<long>(local,"C_CSPlayerPawn","m_pAimPunchServices"),"CCSPlayer_AimPunchServices","m_predictableBaseAngle"),new Vector2(float.NaN));
        bool inputReady=sensitivity is >.01f and <100 && scale is >.01f and <10 && Geometry.Valid(punch)&&Math.Abs(punch.X)<90&&Math.Abs(punch.Y)<90;
        float fov=p.Optional(()=>Get<uint>(Field<long>(local,"C_BasePlayerPawn","m_pCameraServices"),"CCSPlayerBase_CameraServices","m_iFOV"),90u);
        if(fov<1||fov>150)fov=90;
        float hfov=(float)(2*Math.Atan(Math.Tan(fov*Math.PI/360)*(rect.Width/(double)Math.Max(1,rect.Height))/(4.0/3))*180/Math.PI);
        int localIndex=-1;List<long> controllers=[];
        for(uint i=1;i<=64;i++){long address=Entity(i);if(address==ctrl)localIndex=(int)i-1;else if(Memory.Pointer(address))controllers.Add(address);}
        List<Player> players=[];HashSet<long> seen=[];int skipped=0;
        foreach(long controller in controllers)
        {
            try{
                long other=Entity(Get<uint>(controller,"CCSPlayerController","m_hPlayerPawn"));if(!Memory.Pointer(other)||other==pawn)continue;
                var data=p.Read(other,size);int health=Field<int>(data,"C_BaseEntity","m_iHealth"),t=Field<byte>(data,"C_BaseEntity","m_iTeamNum");
                if(health<=0||health>100||t is not (2 or 3)||Field<byte>(data,"C_BaseEntity","m_lifeState")!=0)continue;
                if(t==team&&!cfg.Teammates)continue;
                long scene=Field<long>(data,"C_BaseEntity","m_pGameSceneNode");
                if(Get<byte>(scene,"CGameSceneNode","m_bDormant")!=0)continue;
                var feet=Get<Vector3>(scene,"CGameSceneNode","m_vecAbsOrigin");float height=Field<Vector3>(data,"C_BaseModelEntity","m_vecViewOffset").Z;
                if(!Geometry.Valid(feet)||height is <20 or >80)continue;
                float distance=Vector3.Distance(origin,feet)*.0254f;if(distance>cfg.EspRange)continue;
                var bones=cfg.Skeleton||cfg.Aim||cfg.HeadDot?Bones(scene,feet):[];int rig=Rig(bones,feet,cfg.Rig);
                var head=bones.Length>0&&Geometry.Valid(bones[Geometry.Head(rig)])?bones[Geometry.Head(rig)]:feet+new Vector3(0,0,height);
                ulong mask=BitConverter.ToUInt64(data,o.F("C_CSPlayerPawn","m_entitySpottedState")+o.F("EntitySpottedState_t","m_bSpottedByMask"));
                int? money=cfg.Money?p.Optional<int?>(()=>{int value=Get<int>(Get<long>(controller,"CCSPlayerController","m_pInGameMoneyServices"),"CCSPlayerController_InGameMoneyServices","m_iAccount");return value is >=0 and <=100000?value:null;},null):null;
                players.Add(new(other,Name(controller),health,Field<int>(data,"C_CSPlayerPawn","m_ArmorValue"),money,t,feet,head,bones,rig,localIndex>=0&&(mask&(1ul<<localIndex))!=0,distance,Field<byte>(data,"C_CSPlayerPawn","m_bGunGameImmunity")!=0));
                if(cfg.Steps&&t!=team){seen.Add(other);p.Optional(()=>{long movement=Field<long>(data,"C_BasePlayerPawn","m_pMovementServices");int side=Get<int>(movement,"CPlayer_MovementServices_Humanoid","m_nStepside");
                    var velocity=Field<Vector3>(data,"C_BaseEntity","m_vecAbsVelocity");bool ground=(Field<uint>(data,"C_BaseEntity","m_fFlags")&1)!=0;
                    if(stepState.TryGetValue(other,out var old)&&old.Side!=side&&ground&&velocity.Length()>80&&Vector3.Distance(old.Pos,feet)<100&&Field<byte>(data,"C_CSPlayerPawn","m_bIsWalking")==0)steps.Add(new(feet,start));
                    stepState[other]=(side,feet);return true;},false);}
            }catch{skipped++;}
        }
        foreach(var key in stepState.Keys.Where(k=>!seen.Contains(k)).ToArray())stepState.Remove(key);
        steps.RemoveAll(s=>start-s.Time>1.2);if(steps.Count>128)steps.RemoveRange(0,steps.Count-128);
        if(!cfg.Steps){steps.Clear();stepState.Clear();}
        if(start-extrasAt>.15){extrasAt=start;
            spectators=cfg.Spectators?Spectators(controllers,pawn):[];
            bomb=cfg.Bomb?Bomb(Field<float>(local,"C_BaseEntity","m_flSimulationTime")):null;
            if(cfg.WatermarkMap)map=p.Optional(()=>{long globals=Global("dwGlobalVars");foreach(int off in new[]{0x180,0x188}){string s=p.Optional(()=>p.Text(p.Ptr(globals+off),64),"");if(s.Contains("de_")||s.Contains("cs_")||s.Contains("ar_"))return Geometry.CleanMap(s);}return "";},"");
        }
        int cross=Field<int>(local,"C_CSPlayerPawn","m_iIDEntIndex");
        return new(){Time=start,Pid=p.Pid,LocalTeam=team,Active=true,Alive=alive,InputReady=inputReady,Rect=rect,Eye=eye,Angles=angles,Punch=punch,
            Matrix=matrix,Velocity=Field<Vector3>(local,"C_BaseEntity","m_vecAbsVelocity"),Grounded=(Field<uint>(local,"C_BaseEntity","m_fFlags")&1)!=0,
            MoveType=Field<byte>(local,"C_BaseEntity","m_MoveType"),Sensitivity=sensitivity,FovScale=scale,HFov=hfov,
            Shots=Field<int>(local,"C_CSPlayerPawn","m_iShotsFired"),Crosshair=cross,CrosshairPawn=cross>0?p.Optional(()=>Entity((uint)cross),0L):0,
            Players=players.Where(x=>x.Team!=team||cfg.Teammates).ToArray(),Spectators=spectators,Steps=steps.ToArray(),Bomb=bomb,
            Nick=Name(ctrl),Map=map,Status=$"BUILD {o.Build}  /  {players.Count} PLAYERS"+(skipped>0?$"  /  {skipped} READ SKIPS":"")+(!inputReady?"  /  INPUT DATA UNAVAILABLE":""),
            ReadMs=(Clock.Now-start)*1000};
    }
    string[] Spectators(List<long> controllers,long local)
    {
        List<string> result=[];
        foreach(long controller in controllers)p.Optional(()=>{
            long observer=Entity(Get<uint>(controller,"CCSPlayerController","m_hObserverPawn"));
            if(!Memory.Pointer(observer))observer=Entity(Get<uint>(controller,"CBasePlayerController","m_hPawn"));
            if(!Memory.Pointer(observer))return false;
            long svc=Get<long>(observer,"C_BasePlayerPawn","m_pObserverServices");
            int mode=Get<byte>(svc,"CPlayer_ObserverServices","m_iObserverMode");
            if(mode is 2 or 3&&Entity(Get<uint>(svc,"CPlayer_ObserverServices","m_hObserverTarget"))==local)result.Add(Name(controller));
            return true;
        },false);
        return result.Distinct().ToArray();
    }
    BombInfo? Bomb(float gameTime)
    {
        return p.Optional<BombInfo?>(()=>{
            long pointer=Global("dwPlantedC4");long planted=p.Optional(()=>p.Ptr(pointer),0L);
            foreach(long candidate in new[]{planted,pointer}){
                var found=p.Optional<BombInfo?>(()=>{
                    if(Get<byte>(candidate,"C_PlantedC4","m_bBombTicking")!=1)return null;
                    var pos=Position(candidate);if(!Geometry.Valid(pos))return null;
                    bool defused=Get<byte>(candidate,"C_PlantedC4","m_bBombDefused")!=0;
                    float remain=Get<float>(candidate,"C_PlantedC4","m_flC4Blow")-gameTime;
                    float? defuse=Get<byte>(candidate,"C_PlantedC4","m_bBeingDefused")!=0?Get<float>(candidate,"C_PlantedC4","m_flDefuseCountDown")-gameTime:null;
                    int site=Get<int>(candidate,"C_PlantedC4","m_nBombSite");
                    return new(pos,$"C4 / {(site==0?"A":site==1?"B":"?")}",remain is >=0 and <=90?remain:null,defuse is >=0 and <=15?defuse:null,defused);
                },null);if(found!=null)return found;
            }
            long weapon=Global("dwWeaponC4");
            if(!Memory.Pointer(weapon))return null;
            uint owner=Get<uint>(weapon,"C_BaseEntity","m_hOwnerEntity");long carrier=Entity(owner);
            var position=Position(Memory.Pointer(carrier)?carrier:weapon);
            return Geometry.Valid(position)?new(position,Memory.Pointer(carrier)?"C4 / CARRIER":"C4 / DROPPED"):null;
        },null);
    }
}
