using System.IO;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;

namespace Vertigo;

// Sparse synthetic memory; no process handles and no input injection.
public static class ReaderTests
{
    sealed class Fake:IGameMemory
    {
        public int Pid=>99;public long Client=>0x10000000;public long Engine=>0x20000000;
        public readonly Dictionary<long,byte[]> Blocks=[];readonly Offsets o;
        public const long Local=0x30000000,Enemy=0x31000000,Controller=0x32000000,EnemyController=0x33000000;
        public const long Watcher=0x34000000,Observer=0x35000000,Services=0x36000000;
        public const long Money=0x37000000,Move=0x38000000,Bomb=0x39000000;
        public bool Focus=true;
        public Fake(Offsets offsets,int stride=0x70)
        {
            o=offsets;Put(Engine+o.G("dwBuildNumber","engine2.dll"),o.Build);
            Put(Client+o.G("dwLocalPlayerPawn"),Local);Put(Client+o.G("dwLocalPlayerController"),Controller);
            Put(Client+o.G("dwEntityList"),0x40000000L);Put(0x40000010,0x41000000L);
            Blocks[0x41000000]=new byte[0x78*512];
            foreach(var(i,ptr)in new(int,long)[]{(1,Controller),(2,EnemyController),(3,Watcher),(100,Local),(101,Enemy),(102,Observer)})Put(0x41000000+i*stride,ptr);
            foreach(long ctrl in new[]{Controller,EnemyController,Watcher})Blocks[ctrl]=new byte[3000];
            Field(Controller,"CCSPlayerController","m_hPlayerPawn",100u);Field(EnemyController,"CCSPlayerController","m_hPlayerPawn",101u);
            Field(Watcher,"CCSPlayerController","m_hObserverPawn",102u);
            foreach(var(ctrl,name)in new[]{(Controller,"local"),(EnemyController,"enemy"),(Watcher,"watcher")})
                Encoding.UTF8.GetBytes(name).CopyTo(Blocks[ctrl],o.F("CBasePlayerController","m_iszPlayerName"));
            foreach(var(pawn,team,scene,x)in new(long,byte,long,float)[]{(Local,2,0x50000000,0),(Enemy,3,0x51000000,100)}){
                Blocks[pawn]=new byte[15000];Blocks[scene]=new byte[1024];
                Field(pawn,"C_BaseEntity","m_iHealth",100);Field(pawn,"C_BaseEntity","m_iTeamNum",team);
                Field(pawn,"C_BaseEntity","m_pGameSceneNode",scene);Field(pawn,"C_BaseEntity","m_flSimulationTime",100f);
                Field(pawn,"C_BaseEntity","m_fFlags",1u);Field(pawn,"C_BaseEntity","m_MoveType",(byte)2);
                Field(pawn,"C_BaseEntity","m_vecAbsVelocity",new Vector3(200,0,0));
                Field(pawn,"C_BaseModelEntity","m_vecViewOffset",new Vector3(0,0,64));
                Field(pawn,"C_BasePlayerPawn","m_flFOVSensitivityAdjust",1f);
                Field(scene,"CGameSceneNode","m_vecAbsOrigin",new Vector3(x,0,0));
                Put(pawn+o.F("C_CSPlayerPawn","m_entitySpottedState")+o.F("EntitySpottedState_t","m_bSpottedByMask"),1ul);
            }
            Field(Local,"C_CSPlayerPawn","m_iIDEntIndex",101);
            Field(Local,"C_CSPlayerPawn","m_pAimPunchServices",0x52000000L);
            Blocks[0x52000000]=new byte[256];Field(0x52000000,"CCSPlayer_AimPunchServices","m_predictableBaseAngle",new Vector2(.1f,.2f));
            Put(Client+o.G("dwSensitivity"),0x53000000L);Put(0x53000000+o.G("dwSensitivity_sensitivity"),1f);
            Put(Client+o.G("dwViewAngles"),Vector2.Zero);
            float[] matrix=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];Blocks[Client+o.G("dwViewMatrix")]=MemoryMarshal.AsBytes(matrix.AsSpan()).ToArray();
            Blocks[Money]=new byte[128];Field(EnemyController,"CCSPlayerController","m_pInGameMoneyServices",Money);Field(Money,"CCSPlayerController_InGameMoneyServices","m_iAccount",4200);
            Blocks[Move]=new byte[800];Field(Enemy,"C_BasePlayerPawn","m_pMovementServices",Move);
            Blocks[Observer]=new byte[6000];Blocks[Services]=new byte[128];Field(Observer,"C_BasePlayerPawn","m_pObserverServices",Services);
            Field(Services,"CPlayer_ObserverServices","m_iObserverMode",(byte)2);Field(Services,"CPlayer_ObserverServices","m_hObserverTarget",100u);
            Put(Client+o.G("dwPlantedC4"),0x54000000L);Put(0x54000000,Bomb);Blocks[Bomb]=new byte[7000];Blocks[0x55000000]=new byte[1024];
            Field(Bomb,"C_BaseEntity","m_pGameSceneNode",0x55000000L);Field(0x55000000,"CGameSceneNode","m_vecAbsOrigin",new Vector3(200,0,0));
            Field(Bomb,"C_PlantedC4","m_bBombTicking",(byte)1);Field(Bomb,"C_PlantedC4","m_flC4Blow",125f);
            Field(Bomb,"C_PlantedC4","m_bBeingDefused",(byte)1);Field(Bomb,"C_PlantedC4","m_flDefuseCountDown",105f);
            Put(Client+o.G("dwWeaponC4"),0L);
        }
        public void Field<T>(long ptr,string cls,string key,T value)where T:unmanaged=>Put(ptr+o.F(cls,key),value);
        public void Put<T>(long address,T value)where T:unmanaged
        {
            byte[] data=new byte[Marshal.SizeOf<T>()];MemoryMarshal.Write(data.AsSpan(),in value);
            foreach(var(baseAddress,block)in Blocks)if(address>=baseAddress&&address+data.Length<=baseAddress+block.Length){data.CopyTo(block,address-baseAddress);return;}
            Blocks[address]=data;
        }
        public byte[] Read(long address,int size)
        {foreach(var(baseAddress,data)in Blocks)if(address>=baseAddress&&address+size<=baseAddress+data.Length)return data.AsSpan((int)(address-baseAddress),size).ToArray();throw new InvalidOperationException($"Missing fixture 0x{address:X}/{size}");}
        public T Get<T>(long p)where T:unmanaged=>MemoryMarshal.Read<T>(Read(p,Marshal.SizeOf<T>()));
        public long Ptr(long p)=>Get<long>(p);
        public string Text(long p,int n=128)=>Encoding.UTF8.GetString(Read(p,n)).Split('\0')[0];
        public T Optional<T>(Func<T> action,T fallback){try{return action();}catch{return fallback;}}
        public (Native.Rect,bool) Window()=>(new(){Right=1920,Bottom=1080},Focus);
    }
    public static void Run(Offsets o)
    {
        void Check(bool value,string why)=>SelfTests.Check(value,"reader: "+why);
        foreach(int stride in new[]{0x70,0x78}){
            var memory=new Fake(o,stride);var reader=new Reader(memory,o);var cfg=new Config();var f=reader.Sample(cfg);
            Check(f.Active&&f.Alive&&f.InputReady,"valid local sample");
            Check(f.Players.Length==1&&f.Players[0].Name=="enemy","entity stride and name");
            Check(f.Players[0].Money==4200&&f.Players[0].Spotted,"money and spotted mask");
            Check(f.Spectators.SequenceEqual(new[]{"watcher"}),"Source 2 observer mode 2");
            Check(f.Bomb?.Remaining==25&&f.Bomb.DefuseRemaining==5,"bomb double indirection and timer");
            Check(f.CrosshairPawn==Fake.Enemy,"crosshair entity resolution");
            memory.Field(Fake.Move,"CPlayer_MovementServices_Humanoid","m_nStepside",1);
            Check(reader.Sample(cfg).Steps.Length==1,"step transition emits a marker");
            memory.Blocks.Remove(Fake.Money);Check(reader.Sample(cfg).Players[0].Money==null,"unknown money stays unknown");
            memory.Blocks.Remove(0x52000000);var noPunch=reader.Sample(cfg);Check(noPunch.Players.Length==1&&!noPunch.InputReady,"RCS read failure does not hide ESP");
            memory.Focus=false;Check(!reader.Sample(cfg).Active,"foreground gate");memory.Focus=true;
            memory.Field(Fake.Enemy,"C_BaseEntity","m_iTeamNum",(byte)2);Check(reader.Sample(cfg).Players.Length==0,"teammates filtered");
            Check(reader.Sample(cfg with{Teammates=true}).Players.Length==1,"optional teammate ESP");
            memory.Field(Fake.Local,"C_BaseEntity","m_iHealth",0);Check(!reader.Sample(cfg).Alive,"dead player cannot emit input");
        }
        var mismatch=new Fake(o);mismatch.Put(mismatch.Engine+o.G("dwBuildNumber","engine2.dll"),o.Build+1);
        try{_ = new Reader(mismatch,o);throw new Exception("Mismatched build accepted");}catch(InvalidDataException){Check(true,"build mismatch blocks reader");}
    }
}
