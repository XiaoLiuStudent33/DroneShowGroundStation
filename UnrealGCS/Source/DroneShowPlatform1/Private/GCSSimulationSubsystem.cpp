#include "GCSSimulationSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "CesiumGeoreference.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include <atomic>
#include <random>
THIRD_PARTY_INCLUDES_START
#include "../../../../ConsoleGCS/mavlink/mdhk/mavlink.h"
THIRD_PARTY_INCLUDES_END

// One thread owns all sockets and packet state; the game thread only reads counters.
class FGCSSimulationWorker final : public FRunnable
{
 struct FDrone { uint32 Mode=0; bool Controlled=false, Armed=false, Climbing=false; int32 TargetHeight=0; int32 Height=0; double LastUpdate=0; FSocket* Socket = nullptr; mavlink_status_t Tx{}, Rx{}; mavlink_message_t Scratch{}; uint8 Red=0, Green=180, Blue=255; int32 Lat=0, Lon=0; };
 TArray<FDrone> Drones;
 ISocketSubsystem* Net;
 FRunnableThread* Thread = nullptr;
 int32 TargetPort, Count;
 double Latitude, Longitude, Spacing;
 std::atomic<bool> Cancel{false};
public:
 std::atomic<int32> Created{0};
 std::atomic<uint64> Sent{0}, Errors{0};
 std::atomic<bool> Ready{false}, Failed{false};
 FGCSSimulationWorker(int32 P,int32 N,double Lat,double Lon,double InSpacing)
  : Net(ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)),TargetPort(P),Count(N),Latitude(Lat),Longitude(Lon),Spacing(InSpacing) {}
 bool Launch() { Thread=FRunnableThread::Create(this,TEXT("GCSLocalSimulation")); return Thread!=nullptr; }
 virtual void Stop() override { Cancel=true; }
 ~FGCSSimulationWorker() { Stop(); if(Thread) { Thread->WaitForCompletion(); delete Thread; } }
 virtual uint32 Run() override
 {
  auto Target=Net->CreateInternetAddr(); bool Valid=false; Target->SetIp(TEXT("127.0.0.1"),Valid); Target->SetPort(TargetPort);
  auto Local=Net->CreateInternetAddr(); Local->SetIp(TEXT("127.0.0.1"),Valid);
  // Explicit source ports allow 20K clients beyond Windows' default ephemeral range.
  int32 SourcePort=20000;
  const int32 Columns=FMath::CeilToInt(FMath::Sqrt(static_cast<double>(Count)));
  const double CosLat=FMath::Cos(FMath::DegreesToRadians(Latitude));
  Drones.Reserve(Count);
  for(int32 I=0; I<Count && !Cancel; ++I)
  {
   FSocket* Socket=Net->CreateSocket(NAME_DGram,TEXT("GCS simulated drone"),false);
   if(!Socket) { Failed=true; break; }
   Socket->SetNonBlocking(true);
   bool Bound=false;
   while(SourcePort<=65535 && !Cancel)
   {
    const int32 Candidate=SourcePort++;
    if(Candidate==TargetPort) continue;
    Local->SetPort(Candidate);
    if(Socket->Bind(*Local)) { Bound=true; break; }
   }
   if(!Bound) { Net->DestroySocket(Socket); Failed=true; break; }
   FDrone D; D.Socket=Socket; 
   // Spacing is captured at start and shared by rows/columns. GPS altitude is AMSL.
   D.Lat=FMath::RoundToInt((Latitude+(I/Columns)*Spacing/6378137.0*180.0/PI)*1e7);
   D.Lon=FMath::RoundToInt((Longitude+(I%Columns)*Spacing/(6378137.0*CosLat)*180.0/PI)*1e7);
   Drones.Add(D); Created= Drones.Num();
  }
  if(Drones.Num()!=Count) Failed=true;
  Ready=!Failed && !Cancel;
  const double Start=FPlatformTime::Seconds();
  double CycleStart=Start; int32 Cursor=0;
  std::mt19937 Random(5489u);
  std::uniform_int_distribution<int32> Heights(-1000,6000);
  uint8 Packet[1024], Discard[2048];
  while(!Cancel && !Failed)
  {
   const double Now=FPlatformTime::Seconds();
   const int32 Due=FMath::Clamp(FMath::FloorToInt((Now-CycleStart)*Count)+1,0,Count);
   int32 Budget=128;
   while(Cursor<Due && Budget-->0 && !Cancel)
   {
    FDrone& D=Drones[Cursor++];
    int32 Received=0;
    // Only accept commands from our configured loopback backend endpoint.
    auto From=Net->CreateInternetAddr();
    for(int32 R=0; R<4 && D.Socket->RecvFrom(Discard,sizeof(Discard),Received,*From); ++R) {
     if(From->ToString(false)!=TEXT("127.0.0.1") || From->GetPort()!=TargetPort) continue;
     mavlink_message_t Incoming{}; mavlink_status_t Result{};
     for(int32 B=0; B<Received; ++B) if(mavlink_frame_char_buffer(&D.Scratch,&D.Rx,Discard[B],&Incoming,&Result)==MAVLINK_FRAMING_OK) {
      if(Incoming.msgid==MAVLINK_MSG_ID_LED_CONTROL) {
      mavlink_led_control_t Led{}; mavlink_msg_led_control_decode(&Incoming,&Led);
      if((Led.target_system==0||Led.target_system==1)&&(Led.target_component==0||Led.target_component==1)&&Led.custom_len==3&&Led.instance==255&&Led.pattern==255) {
       D.Red=Led.custom_bytes[0]; D.Green=Led.custom_bytes[1]; D.Blue=Led.custom_bytes[2];
      }
      } else if(Incoming.msgid==MAVLINK_MSG_ID_SET_MODE) {
       mavlink_set_mode_t Mode{}; mavlink_msg_set_mode_decode(&Incoming,&Mode);
       if((Mode.target_system==0||Mode.target_system==1)&&(Mode.base_mode&MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) && (Mode.custom_mode==0||Mode.custom_mode==4||Mode.custom_mode==5||Mode.custom_mode==6||Mode.custom_mode==9)) {
        D.Mode=Mode.custom_mode; D.Climbing=false; D.Controlled=true; D.Height=FMath::Max(0,D.Height);
       }
      } else if(Incoming.msgid==MAVLINK_MSG_ID_COMMAND_LONG) {
       mavlink_command_long_t Cmd{}; mavlink_msg_command_long_decode(&Incoming,&Cmd);
       if((Cmd.target_system!=0&&Cmd.target_system!=1)||(Cmd.target_component!=0&&Cmd.target_component!=1)) continue;
       uint8 ResultCode=MAV_RESULT_UNSUPPORTED;
       if(Cmd.command==MAV_CMD_COMPONENT_ARM_DISARM) {
        ResultCode=MAV_RESULT_DENIED;
        if(Cmd.param1==1.0f && Cmd.param2==0.0f) {
         if(!D.Armed) { D.Height=0; D.TargetHeight=0; D.Climbing=false; }
         D.Armed=true; D.Controlled=true; ResultCode=MAV_RESULT_ACCEPTED;
        } else if(Cmd.param1==0.0f && Cmd.param2==0.0f && D.Height<=0) {
         D.Armed=false; D.Controlled=true; D.Climbing=false; D.Height=0; ResultCode=MAV_RESULT_ACCEPTED;
        }
       } else if(Cmd.command==MAV_CMD_NAV_TAKEOFF) {
        ResultCode=MAV_RESULT_DENIED;
        if(D.Armed&&D.Mode==4&&FMath::IsFinite(Cmd.param7)&&Cmd.param7>0&&Cmd.param7<=100) {
         D.TargetHeight=FMath::RoundToInt(Cmd.param7*1000); D.Climbing=true; D.Controlled=true; ResultCode=MAV_RESULT_ACCEPTED;
        }
       } else if(Cmd.command==MAV_CMD_NAV_LOITER_UNLIM) {
        D.Mode=5; D.Climbing=false; D.Controlled=true; D.Height=FMath::Max(0,D.Height); ResultCode=MAV_RESULT_ACCEPTED;
       } else if(Cmd.command==MAV_CMD_NAV_RETURN_TO_LAUNCH||Cmd.command==MAV_CMD_NAV_LAND) {
        D.Mode=Cmd.command==MAV_CMD_NAV_RETURN_TO_LAUNCH?6:9; D.Climbing=false;
        D.Controlled=true; D.Height=FMath::Max(0,D.Height); ResultCode=MAV_RESULT_ACCEPTED;
       }
       mavlink_command_ack_t Ack{}; Ack.command=Cmd.command; Ack.result=ResultCode; Ack.target_system=Incoming.sysid; Ack.target_component=Incoming.compid;
       mavlink_message_t Reply{}; mavlink_msg_command_ack_encode_status(1,1,&D.Tx,&Reply,&Ack);
       uint8 ReplyBytes[MAVLINK_MAX_PACKET_LEN]; int32 Written=0;
       const int32 ReplySize=mavlink_msg_to_send_buffer(ReplyBytes,&Reply);
       D.Socket->SendTo(ReplyBytes,ReplySize,Written,*Target);
      }
     }
    }
    // BGC2 cluster behavior: fixed XY grid, independently sampled height per drone.
    // One sample feeds both standard MAVLink and vendor #187 in this batch.
    const double T=CycleStart-Start;
    const int32 Lat=D.Lat, Lon=D.Lon;
    const double Phase=(Cursor-1)*0.37;
    const double Delta=D.LastUpdate>0?FMath::Clamp(Now-D.LastUpdate,0.0,2.0):0.0;
    D.LastUpdate=Now;
    if(!D.Controlled) D.Height=Heights(Random);
    else if(D.Mode==6||D.Mode==9) D.Height=FMath::Max(0,D.Height-FMath::RoundToInt(Delta*1000));
    else if(D.Climbing) {
     const int32 Step=FMath::RoundToInt(Delta*1000);
     D.Height += FMath::Clamp(D.TargetHeight-D.Height,-Step,Step);
     if(D.Height==D.TargetHeight) D.Climbing=false;
    }
    const int32 RelativeAlt=D.Height;
    const int32 Alt=30000+RelativeAlt;
    uint16 Cells[4]; int32 Voltage=0; uint16 MinCell=65535,MaxCell=0;
    for(int32 C=0;C<4;++C) { Cells[C]=3950+FMath::RoundToInt(30*FMath::Sin(T*0.3+Phase+C)); Voltage+=Cells[C]; MinCell=FMath::Min(MinCell,Cells[C]); MaxCell=FMath::Max(MaxCell,Cells[C]); }
    const int8 Remaining=static_cast<int8>(FMath::Max(20,100-static_cast<int32>(T/30)));
    mavlink_message_t Message{}; int32 Length=0;
    mavlink_heartbeat_t HB{}; HB.type=MAV_TYPE_QUADROTOR; HB.autopilot=MAV_AUTOPILOT_ARDUPILOTMEGA;
    HB.custom_mode=D.Mode; HB.base_mode=MAV_MODE_FLAG_CUSTOM_MODE_ENABLED|(D.Armed?MAV_MODE_FLAG_SAFETY_ARMED:0); HB.system_status=D.Armed?MAV_STATE_ACTIVE:MAV_STATE_STANDBY; HB.mavlink_version=3;
    mavlink_msg_heartbeat_encode_status(1,1,&D.Tx,&Message,&HB);
    Length+=mavlink_msg_to_send_buffer(Packet+Length,&Message);
    mavlink_gps_raw_int_t GPS{}; GPS.lat=Lat; GPS.lon=Lon; GPS.alt=Alt;
    GPS.fix_type=6; GPS.satellites_visible=30; GPS.eph=80; GPS.epv=120;
    GPS.time_usec=static_cast<uint64>((Now-Start)*1e6);
    mavlink_msg_gps_raw_int_encode_status(1,1,&D.Tx,&Message,&GPS);
    Length+=mavlink_msg_to_send_buffer(Packet+Length,&Message);
    mavlink_global_position_int_t Pos{}; Pos.lat=Lat; Pos.lon=Lon; Pos.alt=Alt; Pos.relative_alt=RelativeAlt;
    Pos.time_boot_ms=static_cast<uint32>((Now-Start)*1000); Pos.hdg=0;
    mavlink_msg_global_position_int_encode_status(1,1,&D.Tx,&Message,&Pos);
    Length+=mavlink_msg_to_send_buffer(Packet+Length,&Message);
    mavlink_sys_status_t Sys{}; Sys.voltage_battery=Voltage; Sys.current_battery=0; Sys.battery_remaining=Remaining;
    mavlink_msg_sys_status_encode_status(1,1,&D.Tx,&Message,&Sys);
    Length+=mavlink_msg_to_send_buffer(Packet+Length,&Message);
    int32 Bytes=0;
    // Use the same vendor dialect as the receiver. These are synthetic status values.
    mavlink_mdhk_status_t Status{};
    Status.time_boot_ms=Pos.time_boot_ms; Status.schema_version=1;
    const FDateTime UTC=FDateTime::UtcNow();
    Status.gps_year=UTC.GetYear(); Status.gps_month=UTC.GetMonth(); Status.gps_day=UTC.GetDay();
    Status.gps_hour=UTC.GetHour(); Status.gps_minute=UTC.GetMinute(); Status.gps_second=UTC.GetSecond();
    Status.latitude=GPS.lat; Status.longitude=GPS.lon; Status.gps_alt=GPS.alt; Status.relative_alt=Pos.relative_alt;
    Status.battery_remaining=Remaining; Status.battery_temperature=2500+FMath::RoundToInt(100*FMath::Sin(T*0.2+Phase)); Status.battery_current=0;
    for(int32 C=0;C<4;++C) Status.cell_voltages[C]=Cells[C];
    Status.battery_cycle_count=10; Status.cell_voltage_delta=MaxCell-MinCell;
    Status.gps_fix_type=6; Status.satellites_visible=30;
    Status.ekf_flags=EKF_ATTITUDE|EKF_VELOCITY_HORIZ|EKF_VELOCITY_VERT|EKF_POS_HORIZ_ABS|EKF_POS_VERT_ABS;
    Status.freemem32=262144; Status.compass_variance=0.01f; Status.mag_field1=450; Status.mag_field2=452;
    Status.rgb_red=D.Red; Status.rgb_green=D.Green; Status.rgb_blue=D.Blue;
    // Keep undocumented validity bits and fictional firmware identifiers unset.
    mavlink_msg_mdhk_status_encode_status(1,1,&D.Tx,&Message,&Status);
    Length+=mavlink_msg_to_send_buffer(Packet+Length,&Message);
    if(D.Socket->SendTo(Packet,Length,Bytes,*Target) && Bytes==Length) ++Sent; else ++Errors;
   }
   if(Cursor==Count && Now-CycleStart>=1.0) { Cursor=0; CycleStart=Now; }
   FPlatformProcess::Sleep(0.001f);
  }
  for(FDrone& D:Drones) { D.Socket->Close(); Net->DestroySocket(D.Socket); }
  Drones.Empty(); return 0;
 }
};

void UGCSSimulationSubsystem::BindUI(UUserWidget* Widget)
{
 UnbindUI(); if(!Widget) return;
 StartButton=Cast<UButton>(Widget->GetWidgetFromName(TEXT("BTN_StartSimulation")));
 StopButton=Cast<UButton>(Widget->GetWidgetFromName(TEXT("BTN_StopSimulation")));
 PortInput=Cast<USpinBox>(Widget->GetWidgetFromName(TEXT("SPN_SimTelemetryPort")));
 CountInput=Cast<USpinBox>(Widget->GetWidgetFromName(TEXT("SPN_SimDroneCount")));
 SpacingInput=Cast<USpinBox>(Widget->GetWidgetFromName(TEXT("SPN_SimSpacingMeters")));
 StatusText=Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("TXT_SimStatus")));
 if(StartButton.IsValid()) StartButton->OnClicked.AddUniqueDynamic(this,&UGCSSimulationSubsystem::OnStart);
 if(StopButton.IsValid()) StopButton->OnClicked.AddUniqueDynamic(this,&UGCSSimulationSubsystem::OnStop);
 for(auto Input:{PortInput,CountInput}) if(Input.IsValid()) { Input->SetMinValue(1); Input->SetMinFractionalDigits(0); Input->SetMaxFractionalDigits(0); Input->SetDelta(1); }
 if(PortInput.IsValid()) { PortInput->SetMaxValue(65535); PortInput->SetValue(Port); }
 if(CountInput.IsValid()) { CountInput->SetMaxValue(20000); CountInput->SetValue(DroneCount); }
 if(SpacingInput.IsValid()) {
  SpacingInput->SetMinValue(0.1f); SpacingInput->SetMaxValue(100.0f);
  SpacingInput->SetMinSliderValue(0.1f); SpacingInput->SetMaxSliderValue(100.0f);
  SpacingInput->SetMinFractionalDigits(1); SpacingInput->SetMaxFractionalDigits(2);
  SpacingInput->SetDelta(0.1f); SpacingInput->SetValue(SpacingMeters);
 }
 Refresh();
 if(GetWorld()) GetWorld()->GetTimerManager().SetTimer(StatusTimer,this,&UGCSSimulationSubsystem::Refresh,0.5f,true);
}
void UGCSSimulationSubsystem::UnbindUI()
{
 if(GetWorld()) GetWorld()->GetTimerManager().ClearTimer(StatusTimer);
 if(StartButton.IsValid()) StartButton->OnClicked.RemoveDynamic(this,&UGCSSimulationSubsystem::OnStart);
 if(StopButton.IsValid()) StopButton->OnClicked.RemoveDynamic(this,&UGCSSimulationSubsystem::OnStop);
 StartButton.Reset(); StopButton.Reset(); PortInput.Reset(); CountInput.Reset(); SpacingInput.Reset(); StatusText.Reset();
}
bool UGCSSimulationSubsystem::StartSimulation(int32 TargetPort,int32 Count,float InSpacingMeters)
{
 if(Worker) return false;
 if(TargetPort<1 || TargetPort>65535 || Count<1 || Count>20000) { Status=TEXT("状态：端口或数量超出范围"); Refresh(); return false; }
 if(!FMath::IsFinite(InSpacingMeters)||InSpacingMeters<0.1f||InSpacingMeters>100.0f) { Status=TEXT("状态：排列间距须为 0.1–100 米"); Refresh(); return false; }
 double Lat=31.9324751,Lon=118.6385469;
 if(GetWorld()) for(TActorIterator<ACesiumGeoreference> It(GetWorld()); It; ++It) { Lat=It->GetOriginLatitude(); Lon=It->GetOriginLongitude(); break; }
 if(!FMath::IsFinite(Lat)||!FMath::IsFinite(Lon)||FMath::Abs(Lat)>85||FMath::Abs(Lon)>180) { Status=TEXT("状态：场地经纬度无效"); Refresh(); return false; }
 Port=TargetPort; DroneCount=Count; SpacingMeters=InSpacingMeters;
 Worker=new FGCSSimulationWorker(Port,DroneCount,Lat,Lon,SpacingMeters);
 if(!Worker->Launch()) { delete Worker; Worker=nullptr; Status=TEXT("状态：无法启动仿真线程"); Refresh(); return false; }
 Refresh(); return true;
}
void UGCSSimulationSubsystem::StopSimulation()
{
 if(Worker) { delete Worker; Worker=nullptr; Status=TEXT("状态：已停止，本机仿真后端将在心跳超时后清空模拟机"); }
 Refresh();
}
void UGCSSimulationSubsystem::OnStart()
{
 if(!PortInput.IsValid()||!CountInput.IsValid()) return;
 const float P=PortInput->GetValue(),N=CountInput->GetValue();
 if(!FMath::IsFinite(P)||!FMath::IsFinite(N)||P<1||P>65535||N<1||N>20000||P!=FMath::FloorToFloat(P)||N!=FMath::FloorToFloat(N)) { Status=TEXT("状态：请输入有效整数端口和数量"); Refresh(); return; }
 StartSimulation(static_cast<int32>(P),static_cast<int32>(N),SpacingInput.IsValid()?SpacingInput->GetValue():SpacingMeters);
}
void UGCSSimulationSubsystem::OnStop() { StopSimulation(); }
void UGCSSimulationSubsystem::Refresh()
{
 if(Worker)
 {
  if(Worker->Failed) { delete Worker; Worker=nullptr; Status=TEXT("状态：无法分配全部 UDP 端点，仿真已停止"); }
  else if(!Worker->Ready) Status=FString::Printf(TEXT("状态：正在创建 %d / %d"),Worker->Created.load(),DroneCount);
  else Status=FString::Printf(TEXT("本机仿真 %d 架 · 间距 %.2f 米 → 127.0.0.1:%d\n已发送 %llu 包 · 失败 %llu（不代表后端在线）"),DroneCount,SpacingMeters,Port,Worker->Sent.load(),Worker->Errors.load());
 }
 if(StatusText.IsValid()) { StatusText->SetAutoWrapText(true); StatusText->SetText(FText::FromString(Status)); }
 if(StartButton.IsValid()) StartButton->SetIsEnabled(!Worker && PortInput.IsValid() && CountInput.IsValid());
 if(StopButton.IsValid()) StopButton->SetIsEnabled(Worker!=nullptr);
 if(PortInput.IsValid()) PortInput->SetIsEnabled(!Worker);
 if(CountInput.IsValid()) CountInput->SetIsEnabled(!Worker);
 if(SpacingInput.IsValid()) SpacingInput->SetIsEnabled(!Worker);
}
void UGCSSimulationSubsystem::Deinitialize()
{
 StopSimulation(); UnbindUI(); Super::Deinitialize();
}
