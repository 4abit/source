using System.Diagnostics;
using System.IO;
using System.Numerics;

namespace Vertigo;

public interface IOutput {void Move(int x,int y);void Button(bool down);void Key(int vk,bool down);}
public sealed class SystemOutput:IOutput
{public void Move(int x,int y)=>Native.Move(x,y);public void Button(bool down)=>Native.MouseButton(down);public void Key(int vk,bool down)=>Native.Key(vk,down);}

public sealed class Controller(IOutput output)
{
    Vector2 previousPunch,pending,remainder;int previousShots;long target,triggerTarget;
    double triggerAt,fireUntil,nextFire,jumpUntil,lastJump;bool fireDown,jumpDown;
    readonly HashSet<int> ownedKeys=[];
    void Key(int key,bool desired,Func<int,bool> physical)
    {
        if(desired){if(!ownedKeys.Contains(key)&&!physical(key)){output.Key(key,true);ownedKeys.Add(key);}}
        else if(ownedKeys.Remove(key)&&!physical(key))output.Key(key,false);
    }
    public void Reset(Func<int,bool> physical)
    {
        // Attempt every release even if Windows rejects an earlier one.
        List<Exception> errors=[];
        void Release(Action action){try{action();}catch(Exception ex){errors.Add(ex);}}
        if(fireDown){fireDown=false;if(!physical(1))Release(()=>output.Button(false));}
        if(jumpDown){jumpDown=false;Release(()=>output.Key(0x20,false));}
        foreach(int key in ownedKeys.ToArray())Release(()=>Key(key,false,physical));
        previousPunch=pending=remainder=Vector2.Zero;previousShots=0;target=triggerTarget=0;triggerAt=fireUntil=nextFire=jumpUntil=lastJump=0;
        if(errors.Count>0)throw new AggregateException("Не удалось отпустить часть клавиш",errors);
    }
    static Vector3 Target(Player p,int bone)
    {
        int index=bone==0?Geometry.Head(p.Rig):bone==1?(p.Rig==0?5:6):4;
        return p.Bones.Length>index&&Geometry.Valid(p.Bones[index])?p.Bones[index]:p.Head-new Vector3(0,0,bone*9);
    }
    public void Tick(Frame f,Config c,Func<int,bool> physical,bool enabled,double dt)
    {
        double now=Clock.Now;
        if(!enabled||!f.Active||!f.Alive||now-f.Time>.2){Reset(physical);return;}
        dt=Math.Clamp(dt,.001,.05);
        if(fireDown&&now>=fireUntil){fireDown=false;if(!physical(1))output.Button(false);}
        if(jumpDown&&now>=jumpUntil){jumpDown=false;output.Key(0x20,false);}
        var cross=f.Players.FirstOrDefault(p=>p.Address==f.CrosshairPawn&&p.Team!=f.LocalTeam&&!p.Immune);
        bool trigger=c.Trigger&&physical(c.TriggerKey)&&cross!=null&&(!c.TriggerSpotted||cross.Spotted)&&(!c.TriggerStationary||new Vector2(f.Velocity.X,f.Velocity.Y).Length()<40)&&!physical(1);
        if(!trigger){triggerTarget=0;triggerAt=0;}
        else {
            if(triggerTarget!=cross!.Address){triggerTarget=cross.Address;triggerAt=now;}
            if(!fireDown&&now-triggerAt>=c.TriggerDelay/1000&&now>=nextFire){output.Button(true);fireDown=true;fireUntil=now+.016;nextFire=now+c.TriggerInterval/1000;}
        }
        bool shooting=physical(1)||fireDown;
        if(f.InputReady){
            var recoil=new Vector2((float)(f.Punch.X*2*c.RcsY),(float)(f.Punch.Y*2*c.RcsX));
            Player? chosen=null;
            if(c.Aim&&physical(c.AimKey)&&(!c.AimFireOnly||shooting)){
                var candidates=f.Players.Where(p=>p.Team!=f.LocalTeam&&!p.Immune&&(!c.SpottedOnly||p.Spotted))
                    .Select(p=>(P:p,A:Geometry.Look(f.Eye,Target(p,c.AimBone))))
                    .Select(x=>(x.P,x.A,Error:Math.Sqrt(Math.Pow(Geometry.Wrap(x.A.X-f.Angles.X-f.Punch.X*2),2)+Math.Pow(Geometry.Wrap(x.A.Y-f.Angles.Y-f.Punch.Y*2),2))))
                    .Where(x=>x.Error<=c.AimFov).OrderBy(x=>x.P.Address==target?0:1).ThenBy(x=>x.Error).ToArray();
                if(candidates.Length>0)chosen=candidates[0].P;
            }
            Vector2 correction=Vector2.Zero;
            if(chosen!=null){
                target=chosen.Address;var desired=Geometry.Look(f.Eye,Target(chosen,c.AimBone));
                correction=new((float)Geometry.Wrap(desired.X-f.Angles.X),(float)Geometry.Wrap(desired.Y-f.Angles.Y));
                if(c.Rcs&&shooting&&f.Shots>=c.RcsStart)correction-=recoil;
                correction*=(float)(1-Math.Exp(-dt/(c.AimSmooth/1000)));pending=Vector2.Zero;
            } else {
                target=0;
                if(c.Rcs&&shooting&&f.Shots>=c.RcsStart&&previousShots>0&&f.Shots>=previousShots){
                    var recoilDelta=recoil-previousPunch;
                    if(recoilDelta.Length()<12)pending-=recoilDelta;else pending=Vector2.Zero;
                    float alpha=c.RcsSmooth<=0?1:(float)(1-Math.Exp(-dt/(c.RcsSmooth/1000)));
                    correction=pending*alpha;pending-=correction;
                }else pending=Vector2.Zero;
            }
            previousPunch=recoil;previousShots=shooting?Math.Max(0,f.Shots):0;
            if(correction==Vector2.Zero&&chosen==null&&!(c.Rcs&&shooting))remainder=Vector2.Zero;
            if(Geometry.Valid(correction)){
                // Limit spikes, retain fractional mouse counts between samples.
                float max=(float)(240*dt);if(correction.Length()>max)correction=Vector2.Normalize(correction)*max;
                double ux=c.MouseYaw*f.Sensitivity*f.FovScale,uy=c.MousePitch*f.Sensitivity*f.FovScale;
                var counts=new Vector2((float)(-correction.Y/ux),(float)(correction.X/uy))+remainder;
                int dx=(int)Math.Truncate(counts.X),dy=(int)Math.Truncate(counts.Y);remainder=counts-new Vector2(dx,dy);
                output.Move(dx,dy);
            }
        }else {pending=previousPunch=remainder=Vector2.Zero;previousShots=0;}

        bool movement=physical(c.MoveKey)&&f.MoveType==2; // walk only: never ladder/noclip
        if(c.Bhop&&movement&&f.Grounded&&!jumpDown&&now-lastJump>.09){output.Key(0x20,true);jumpDown=true;jumpUntil=now+.018;lastJump=now;}
        bool strafe=c.Strafe&&movement&&!f.Grounded;
        double speed=new Vector2(f.Velocity.X,f.Velocity.Y).Length();
        double yaw= Math.Atan2(f.Velocity.Y,f.Velocity.X)*180/Math.PI;
        double delta=Geometry.Wrap(f.Angles.Y-yaw);
        // Input-only air steering: use the side toward the current view heading.
        // No server velocity modification, alternating view angles, or fake speed claim.
        bool left=Math.Abs(delta)>1?delta>0:((int)(now*10)&1)==0;
        Key(0x41,strafe&&speed>15&&left,physical);
        Key(0x44,strafe&&speed>15&&!left,physical);
        // Release our forward hold in the air: at speed, forward input can
        // consume the air-acceleration budget instead of adding lateral speed.
        Key(0x57,c.AutoForward&&movement&&(!strafe||speed<80),physical);
    }
}

public sealed class Runtime:IDisposable
{
    public Config Config {get=>Volatile.Read(ref config);set=>Volatile.Write(ref config,value.Sanitize());}
    Config config;Frame frame=new();public Frame Frame=>Volatile.Read(ref frame);
    public volatile bool MenuOpen=true;public bool Demo{get;}
    readonly CancellationTokenSource stop=new();Thread? worker;readonly PhysicalInput? input;
    public Runtime(Config c,bool demo,PhysicalInput? keys){config=c;Demo=demo;input=keys;}
    public void Start(){worker=new Thread(Run){IsBackground=true,Name="Vertigo sampler"};worker.Start();}
    void Run()
    {
        if(Demo)return;
        Memory? memory=null;Reader? reader=null;var controller=new Controller(new SystemOutput());
        string lastError="";double last=Clock.Now;int failures=0;
        bool Physical(int key)=>input?.Down(key)??false;
        try{
            var offsets=new Offsets(Path.Combine(AppContext.BaseDirectory,"Data"));
            while(!stop.IsCancellationRequested){
                double start=Clock.Now;
                try{
                    if(memory==null||!memory.Alive){
                        controller.Reset(Physical);memory?.Dispose();memory=null;reader=null;
                        var processes=Process.GetProcessesByName("cs2");
                        if(processes.Length==0){Volatile.Write(ref frame,new(){Status="Ожидание cs2.exe"});stop.Token.WaitHandle.WaitOne(1000);continue;}
                        foreach(var proc in processes.Skip(1))proc.Dispose();
                        memory=new Memory(processes[0]);reader=new Reader(memory,offsets);Log.Write($"Attached PID {memory.Pid}, build {offsets.Build}");
                    }
                    var c=Config;var next=reader!.Sample(c);Volatile.Write(ref frame,next);
                    controller.Tick(next,c,Physical,!MenuOpen&&Native.Foreground(memory.Pid),start-last);
                    failures=0;lastError="";
                }catch(Exception ex){
                    try{controller.Reset(Physical);}catch(Exception release){Log.Write("Input release: "+release.Message);}
                    if(ex.Message!=lastError){Log.Write(ex.ToString());lastError=ex.Message;}
                    Volatile.Write(ref frame,new(){Status=ex.Message});failures++;
                    if(reader==null||failures>=5||ex is InvalidDataException){memory?.Dispose();memory=null;reader=null;stop.Token.WaitHandle.WaitOne(ex is InvalidDataException?5000:1000);}
                }
                last=start;int wait=(int)Math.Max(1,1000.0/120-(Clock.Now-start)*1000);stop.Token.WaitHandle.WaitOne(wait);
            }
        }catch(Exception ex){Log.Write(ex.ToString());Volatile.Write(ref frame,new(){Status=ex.Message});}
        finally{try{controller.Reset(Physical);}catch(Exception ex){Log.Write(ex.Message);}memory?.Dispose();}
    }
    public void PublishDemo(Frame f)=>Volatile.Write(ref frame,f);
    public void Dispose(){stop.Cancel();worker?.Join(3000);stop.Dispose();}
}
