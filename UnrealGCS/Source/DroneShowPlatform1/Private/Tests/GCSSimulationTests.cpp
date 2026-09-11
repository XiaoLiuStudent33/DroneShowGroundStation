#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include <limits>
#include "GCSSimulationSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
THIRD_PARTY_INCLUDES_START
#include "../../../../../ConsoleGCS/mavlink/mdhk/mavlink.h"
THIRD_PARTY_INCLUDES_END

class FSimWait final : public IAutomationLatentCommand
{
 TFunction<bool()> Work;
public:
 explicit FSimWait(TFunction<bool()> In):Work(MoveTemp(In)){}
 virtual bool Update() override { return Work(); }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSSimulationTest,"DroneGCS.UI.Simulation.LoopbackAndLifecycle",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSSimulationTest::RunTest(const FString&)
{
 auto* Net=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
 auto Address=Net->CreateInternetAddr(); bool Valid=false; Address->SetIp(TEXT("127.0.0.1"),Valid); Address->SetPort(0);
 FSocket* Listener=Net->CreateSocket(NAME_DGram,TEXT("Simulation test sink"),false);
 if(!TestNotNull(TEXT("Sink socket"),Listener)) return false;
 if(!Listener->Bind(*Address)) { Net->DestroySocket(Listener); AddError(TEXT("Cannot bind loopback sink")); return false; }
 Listener->SetNonBlocking(true); Listener->GetAddress(*Address);
 auto* GI=NewObject<UGameInstance>(GEngine); GI->AddToRoot(); GI->InitializeStandalone(TEXT("SimulationTest"));
 auto* Sim=GI->GetSubsystem<UGCSSimulationSubsystem>();
 auto* Class=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_RightPanel.WBP_GCS_RightPanel_C"));
 if(!Class) { GI->Shutdown(); GI->RemoveFromRoot(); Net->DestroySocket(Listener); AddError(TEXT("Missing real RightPanel asset")); return false; }
 auto* Panel=CreateWidget<UUserWidget>(GI,Class); Panel->AddToRoot(); Sim->BindUI(Panel);
 TestTrue(TEXT("Actual UI has all five correctly typed controls"),Sim->StartButton.IsValid()&&Sim->StopButton.IsValid()&&Sim->PortInput.IsValid()&&Sim->CountInput.IsValid()&&Sim->StatusText.IsValid());
 if(!Sim->StartButton.IsValid()||!Sim->StopButton.IsValid()||!Sim->PortInput.IsValid()||!Sim->CountInput.IsValid()) { GI->Shutdown(); GI->RemoveFromRoot(); Panel->RemoveFromRoot(); Net->DestroySocket(Listener); return false; }
 TestFalse(TEXT("Port zero rejected"),Sim->StartSimulation(0,3));
 TestFalse(TEXT("Excess count rejected"),Sim->StartSimulation(Address->GetPort(),20001));
 if(!TestTrue(TEXT("Actual spacing SpinBox found"),Sim->SpacingInput.IsValid())) { GI->Shutdown(); GI->RemoveFromRoot(); Panel->RemoveFromRoot(); Net->DestroySocket(Listener); return false; }
 TestFalse(TEXT("Zero spacing rejected"),Sim->StartSimulation(Address->GetPort(),3,0));
 TestFalse(TEXT("Oversize spacing rejected"),Sim->StartSimulation(Address->GetPort(),3,101));
 TestFalse(TEXT("NaN spacing rejected"),Sim->StartSimulation(Address->GetPort(),3,std::numeric_limits<float>::quiet_NaN()));
 TestEqual(TEXT("Default spacing"),Sim->SpacingInput->GetValue(),0.5f);
 Sim->SpacingInput->SetValue(0.5f);
 Sim->PortInput->SetValue(Address->GetPort()); Sim->CountInput->SetValue(3);
 Sim->StartButton->OnClicked.Broadcast();
 TestTrue(TEXT("Button starts worker"),Sim->IsRunning());
 TestFalse(TEXT("Repeated start rejected"),Sim->StartSimulation(Address->GetPort(),3));
 TestFalse(TEXT("Port frozen while running"),Sim->PortInput->GetIsEnabled());
 TestFalse(TEXT("Spacing frozen while running"),Sim->SpacingInput->GetIsEnabled());
 auto Positions=MakeShared<TMap<int32,FVector2D>>();
 auto Seen=MakeShared<TSet<int32>>(); const double Deadline=FPlatformTime::Seconds()+12;
 auto Samples=MakeShared<TMap<int32,mavlink_mdhk_status_t>>(); auto Changed=MakeShared<bool>(false); auto RedSamples=MakeShared<int32>(0); auto Selected=MakeShared<int32>(0);
 ADD_LATENT_AUTOMATION_COMMAND(FSimWait([this,GI,Panel,Sim,Listener,Net,Seen,Positions,Deadline,Samples,Changed,RedSamples,Selected]()
 {
  uint8 Buffer[2048]; int32 Bytes=0; auto From=Net->CreateInternetAddr(); int32 Budget=100;
  while(Budget-->0 && Listener->RecvFrom(Buffer,sizeof(Buffer),Bytes,*From))
  {
   mavlink_message_t Scratch{},Msg{}; mavlink_status_t State{},Result{}; TSet<uint32> Types; int32 GpsAlt=0, RelativeAlt=0;
   for(int32 I=0;I<Bytes;++I) if(mavlink_frame_char_buffer(&Scratch,&State,Buffer[I],&Msg,&Result)==MAVLINK_FRAMING_OK)
   {
    Types.Add(Msg.msgid);
    if(Msg.msgid==MAVLINK_MSG_ID_MDHK_STATUS) {
     mavlink_mdhk_status_t S{}; mavlink_msg_mdhk_status_decode(&Msg,&S);
     TestEqual(TEXT("187 schema"),S.schema_version,static_cast<uint8>(1));
     TestEqual(TEXT("187 battery percent"),S.battery_remaining,static_cast<int8>(100));
     TestTrue(TEXT("Temperature bounded"),S.battery_temperature>=2400&&S.battery_temperature<=2600);
     for(auto Cell:S.cell_voltages) TestTrue(TEXT("Cell bounded"),Cell>=3920&&Cell<=3980);
     TestTrue(TEXT("187 height range"),S.relative_alt>=-1000&&S.relative_alt<=6000);
     TestEqual(TEXT("187 and standard relative height agree"),S.relative_alt,RelativeAlt);
     TestEqual(TEXT("187 and GPS altitude agree"),S.gps_alt,GpsAlt);
     TestEqual(TEXT("AMSL equals origin plus relative height"),S.gps_alt,30000+S.relative_alt);
     TestTrue(TEXT("187 GPS populated"),S.latitude>319000000 && S.gps_year>=2026);
     if(!Positions->Contains(From->GetPort())) Positions->Add(From->GetPort(),FVector2D(S.latitude/1e7,S.longitude/1e7));
     if(auto* Previous=Samples->Find(From->GetPort())) {
      TestEqual(TEXT("Grid latitude stays fixed"),S.latitude,Previous->latitude);
      TestEqual(TEXT("Grid longitude stays fixed"),S.longitude,Previous->longitude);
      if(Previous->relative_alt!=S.relative_alt && Previous->battery_temperature!=S.battery_temperature) *Changed=true;
     } else Samples->Add(From->GetPort(),S);
     if(*Selected==0) {
      *Selected=From->GetPort();
      uint8 Custom[24]{}; Custom[0]=255;
      mavlink_message_t Led{}; mavlink_msg_led_control_pack(255,190,&Led,0,0,255,255,3,Custom);
      uint8 Out[MAVLINK_MAX_PACKET_LEN]; int32 SentBytes=0;
      Listener->SendTo(Out,mavlink_msg_to_send_buffer(Out,&Led),SentBytes,*From);
     } else if(From->GetPort()==*Selected) {
      if(S.rgb_red==255&&S.rgb_green==0&&S.rgb_blue==0) ++*RedSamples;
      else if(*RedSamples>0) AddError(TEXT("RGB reverted after command"));
     } else TestTrue(TEXT("Unselected drone color unchanged"),S.rgb_red==0&&S.rgb_green==180&&S.rgb_blue==255);
    }
    if(Msg.msgid==MAVLINK_MSG_ID_HEARTBEAT) { mavlink_heartbeat_t H{}; mavlink_msg_heartbeat_decode(&Msg,&H); TestEqual(TEXT("Drone heartbeat"),H.type,static_cast<uint8>(MAV_TYPE_QUADROTOR)); }
    if(Msg.msgid==MAVLINK_MSG_ID_GPS_RAW_INT) { mavlink_gps_raw_int_t G{}; mavlink_msg_gps_raw_int_decode(&Msg,&G); GpsAlt=G.alt; TestTrue(TEXT("AMSL noise bounded"),G.alt>=29000&&G.alt<=36000); TestTrue(TEXT("Site latitude"),G.lat>319000000&&G.lat<320000000); }
    if(Msg.msgid==MAVLINK_MSG_ID_GLOBAL_POSITION_INT) { mavlink_global_position_int_t P{}; mavlink_msg_global_position_int_decode(&Msg,&P); RelativeAlt=P.relative_alt; TestEqual(TEXT("Standard GPS and global altitude agree"),P.alt,GpsAlt); }
   }
   TestEqual(TEXT("Five valid MAVLink messages per packet including vendor status"),Types.Num(),5);
   TestTrue(TEXT("Vendor status included with valid CRC"),Types.Contains(MAVLINK_MSG_ID_MDHK_STATUS));
   if(!Seen->Contains(From->GetPort())) AddInfo(FString::Printf(TEXT("SimulationPacket source=%d payload=%s"),From->GetPort(),*FBase64::Encode(Buffer,Bytes)));
   Seen->Add(From->GetPort());
  }
  if((Seen->Num()<3||!*Changed||*RedSamples<3) && FPlatformTime::Seconds()<Deadline) return false;
  TestTrue(TEXT("Independent height and temperature change over time"),*Changed);
  TestTrue(TEXT("RGB persists over three telemetry cycles"),*RedSamples>=3);
  TestEqual(TEXT("Three independent source endpoints"),Seen->Num(),3);
  TSet<int32> Heights; for(const auto& Sample:*Samples) Heights.Add(Sample.Value.relative_alt);
  TestTrue(TEXT("Drones have distinct initial heights"),Heights.Num()>1);
  TArray<FVector2D> Coordinates; Positions->GenerateValueArray(Coordinates);
  if(Coordinates.Num()==3) {
   double MinLat=Coordinates[0].X,MaxLat=MinLat,MinLon=Coordinates[0].Y,MaxLon=MinLon;
   for(const auto& P:Coordinates) { MinLat=FMath::Min(MinLat,P.X); MaxLat=FMath::Max(MaxLat,P.X); MinLon=FMath::Min(MinLon,P.Y); MaxLon=FMath::Max(MaxLon,P.Y); }
   const double North=FMath::DegreesToRadians(MaxLat-MinLat)*6378137.0;
   const double East=FMath::DegreesToRadians(MaxLon-MinLon)*6378137.0*FMath::Cos(FMath::DegreesToRadians(MinLat));
   TestTrue(TEXT("On-wire row spacing is 0.5m within GPS quantization"),FMath::Abs(North-0.5)<0.02);
   TestTrue(TEXT("On-wire column spacing is 0.5m within GPS quantization"),FMath::Abs(East-0.5)<0.02);
  }
  Sim->BindUI(Panel); TestTrue(TEXT("Rebinding preserves session"),Sim->IsRunning());
  TestEqual(TEXT("Rebinding preserves spacing"),Sim->SpacingInput->GetValue(),0.5f);
  Sim->StopButton->OnClicked.Broadcast(); TestFalse(TEXT("Stop joins worker"),Sim->IsRunning());
  TestTrue(TEXT("Inputs restored"),Sim->PortInput->GetIsEnabled());
  TestTrue(TEXT("Spacing editable after stop"),Sim->SpacingInput->GetIsEnabled());
  auto* Button=Sim->StartButton.Get(); Sim->UnbindUI(); Button->OnClicked.Broadcast(); TestFalse(TEXT("Unbound button cannot restart"),Sim->IsRunning());
  TestTrue(TEXT("Restart after stop"),Sim->StartSimulation(Listener->GetPortNo(),3));
  GI->Shutdown(); GI->RemoveFromRoot(); Panel->RemoveFromRoot(); Listener->Close(); Net->DestroySocket(Listener);
  TestFalse(TEXT("Game instance shutdown stops active worker"),Sim->IsRunning());
  return true;
 }));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSSimCommandsTest,"DroneGCS.UI.Simulation.Commands",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSSimCommandsTest::RunTest(const FString&)
{
 auto* Net=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
 auto Addr=Net->CreateInternetAddr(); bool Valid=false; Addr->SetIp(TEXT("127.0.0.1"),Valid); Addr->SetPort(0);
 auto* Sock=Net->CreateSocket(NAME_DGram,TEXT("Sim command test"),false);
 if(!Sock||!Sock->Bind(*Addr)) { AddError(TEXT("Cannot bind")); if(Sock) Net->DestroySocket(Sock); return false; }
 Sock->SetNonBlocking(true); Sock->GetAddress(*Addr);
 auto* GI=NewObject<UGameInstance>(GEngine); GI->AddToRoot(); GI->InitializeStandalone(TEXT("SimCommands"));
 auto* Sim=GI->GetSubsystem<UGCSSimulationSubsystem>(); Sim->StartSimulation(Addr->GetPort(),1);
 struct FState { int Stage=0, Stable=0; int32 Height=0,LastHeight=0; bool Ack=false; uint32 Mode=0; };
 auto State=MakeShared<FState>(); const double Deadline=FPlatformTime::Seconds()+22;
 ADD_LATENT_AUTOMATION_COMMAND(FSimWait([this,Net,Sock,GI,Sim,State,Deadline]() {
  uint8 Buffer[2048]; int32 Bytes=0; auto From=Net->CreateInternetAddr();
  while(Sock->RecvFrom(Buffer,sizeof(Buffer),Bytes,*From)) {
   mavlink_message_t Scratch{},Msg{}; mavlink_status_t Rx{},Out{};
   bool Telemetry=false;
   for(int I=0;I<Bytes;++I) if(mavlink_frame_char_buffer(&Scratch,&Rx,Buffer[I],&Msg,&Out)==MAVLINK_FRAMING_OK) {
    if(Msg.msgid==MAVLINK_MSG_ID_HEARTBEAT) { mavlink_heartbeat_t H{}; mavlink_msg_heartbeat_decode(&Msg,&H); State->Mode=H.custom_mode; }
    if(Msg.msgid==MAVLINK_MSG_ID_MDHK_STATUS) { mavlink_mdhk_status_t S{}; mavlink_msg_mdhk_status_decode(&Msg,&S); State->Height=S.relative_alt; Telemetry=true; }
    if(Msg.msgid==MAVLINK_MSG_ID_COMMAND_ACK) { mavlink_command_ack_t A{}; mavlink_msg_command_ack_decode(&Msg,&A); if(A.command==MAV_CMD_NAV_RETURN_TO_LAUNCH) State->Ack=A.result==MAV_RESULT_ACCEPTED; }
   }
   if(!Telemetry) continue;
   mavlink_message_t Request{}; bool Send=false;
   if(State->Stage==0) { mavlink_msg_set_mode_pack(255,190,&Request,1,MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,5); State->Stage=1; Send=true; }
   else if(State->Stage==1&&State->Mode==5) { mavlink_msg_set_mode_pack(255,190,&Request,1,MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,0); State->Stage=2; State->LastHeight=State->Height; Send=true; }
   else if(State->Stage==2&&State->Mode==0) {
    TestEqual(TEXT("Manual mode holds altitude across reports"),State->Height,State->LastHeight);
    if(++State->Stable>=3) { mavlink_msg_command_long_pack(255,190,&Request,1,1,MAV_CMD_NAV_RETURN_TO_LAUNCH,0,0,0,0,0,0,0,0); State->Stage=3; Send=true; }
   } else if(State->Stage==3&&State->Mode==6) {
    TestTrue(TEXT("RTL descends monotonically"),State->Height<=State->LastHeight); State->LastHeight=State->Height;
    if(State->Height==0&&State->Ack) State->Stage=4;
   }
   if(Send) { uint8 Data[MAVLINK_MAX_PACKET_LEN]; const int Len=mavlink_msg_to_send_buffer(Data,&Request); int32 Sent=0; Sock->SendTo(Data,Len,Sent,*From); }
  }
  if(State->Stage!=4&&FPlatformTime::Seconds()<Deadline) return false;
  TestEqual(TEXT("Loiter, manual persistence, RTL ACK and landing complete"),State->Stage,4);
  Sim->StopSimulation(); GI->Shutdown(); GI->RemoveFromRoot(); Sock->Close(); Net->DestroySocket(Sock); return true;
 }));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSSimFlightTest,"DroneGCS.UI.Simulation.FlightLifecycle",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSSimFlightTest::RunTest(const FString&)
{
 auto* Net=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
 auto Addr=Net->CreateInternetAddr(); bool Valid=false; Addr->SetIp(TEXT("127.0.0.1"),Valid); Addr->SetPort(0);
 auto* Sock=Net->CreateSocket(NAME_DGram,TEXT("Sim command test"),false);
 if(!Sock||!Sock->Bind(*Addr)) { AddError(TEXT("Cannot bind")); if(Sock) Net->DestroySocket(Sock); return false; }
 Sock->SetNonBlocking(true); Sock->GetAddress(*Addr);
 auto* GI=NewObject<UGameInstance>(GEngine); GI->AddToRoot(); GI->InitializeStandalone(TEXT("SimCommands"));
 auto* Sim=GI->GetSubsystem<UGCSSimulationSubsystem>(); Sim->StartSimulation(Addr->GetPort(),1);
 struct FState { int Stage=0, Stable=0; int32 Height=0,LastHeight=0; bool Ack=false, Armed=false; uint32 Mode=0; };
 auto State=MakeShared<FState>(); const double Deadline=FPlatformTime::Seconds()+30;
 ADD_LATENT_AUTOMATION_COMMAND(FSimWait([this,Net,Sock,GI,Sim,State,Deadline]() {
  uint8 Buffer[2048]; int32 Bytes=0; auto From=Net->CreateInternetAddr();
  while(Sock->RecvFrom(Buffer,sizeof(Buffer),Bytes,*From)) {
   mavlink_message_t Scratch{},Msg{}; mavlink_status_t Rx{},Out{};
   bool Telemetry=false;
   for(int I=0;I<Bytes;++I) if(mavlink_frame_char_buffer(&Scratch,&Rx,Buffer[I],&Msg,&Out)==MAVLINK_FRAMING_OK) {
    if(Msg.msgid==MAVLINK_MSG_ID_HEARTBEAT) { mavlink_heartbeat_t H{}; mavlink_msg_heartbeat_decode(&Msg,&H); State->Mode=H.custom_mode; State->Armed=(H.base_mode&MAV_MODE_FLAG_SAFETY_ARMED)!=0; }
    if(Msg.msgid==MAVLINK_MSG_ID_MDHK_STATUS) { mavlink_mdhk_status_t S{}; mavlink_msg_mdhk_status_decode(&Msg,&S); State->Height=S.relative_alt; Telemetry=true; }
    if(Msg.msgid==MAVLINK_MSG_ID_COMMAND_ACK) { mavlink_command_ack_t A{}; mavlink_msg_command_ack_decode(&Msg,&A); if(A.command==MAV_CMD_NAV_RETURN_TO_LAUNCH) State->Ack=A.result==MAV_RESULT_ACCEPTED; }
   }
   if(!Telemetry) continue;
   mavlink_message_t Request{}; bool Send=false;
   auto Command=[&](uint16 Id,float P1,float Alt) { mavlink_msg_command_long_pack(255,190,&Request,1,1,Id,0,P1,0,0,0,0,0,Alt); Send=true; };
   if(State->Stage==0) { Command(MAV_CMD_COMPONENT_ARM_DISARM,1,0); State->Stage=1; }
   else if(State->Stage==1&&State->Armed) {
    TestEqual(TEXT("Arm initializes ground state"),State->Height,0);
    mavlink_msg_set_mode_pack(255,190,&Request,1,MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,4); Send=true; State->Stage=2;
   } else if(State->Stage==2&&State->Mode==4) { Command(MAV_CMD_NAV_TAKEOFF,0,2000.0f/1000); State->Stage=3; State->LastHeight=0; }
   else if(State->Stage==3) {
    TestTrue(TEXT("Climb is monotonic and bounded"),State->Height>=State->LastHeight&&State->Height<=2000); State->LastHeight=State->Height;
    if(State->Height==2000) { Command(MAV_CMD_NAV_LOITER_UNLIM,0,0); State->Stage=4; }
   } else if(State->Stage==4&&State->Mode==5) {
    TestEqual(TEXT("Hover holds requested height"),State->Height,2000);
    if(++State->Stable>=3) { Command(MAV_CMD_COMPONENT_ARM_DISARM,0,0); State->Stage=5; }
   } else if(State->Stage==5) {
    TestTrue(TEXT("Airborne disarm is rejected"),State->Armed); Command(MAV_CMD_NAV_LAND,0,0); State->Stage=6;
   } else if(State->Stage==6&&State->Mode==9) {
    TestTrue(TEXT("Landing descends"),State->Height<=State->LastHeight); State->LastHeight=State->Height;
    if(State->Height==0) { Command(MAV_CMD_COMPONENT_ARM_DISARM,0,0); State->Stage=7; }
   } else if(State->Stage==7&&!State->Armed) { TestEqual(TEXT("Disarmed on ground"),State->Height,0); State->Stage=8; }
   if(Send) { uint8 Data[MAVLINK_MAX_PACKET_LEN]; const int Len=mavlink_msg_to_send_buffer(Data,&Request); int32 Sent=0; Sock->SendTo(Data,Len,Sent,*From); }
  }
  if(State->Stage!=8&&FPlatformTime::Seconds()<Deadline) return false;
  TestEqual(TEXT("Arm takeoff hover land disarm complete"),State->Stage,8);
  Sim->StopSimulation(); GI->Shutdown(); GI->RemoveFromRoot(); Sock->Close(); Net->DestroySocket(Sock); return true;
 }));
 return true;
}
#endif
